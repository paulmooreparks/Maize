// mzasm_object.cpp (maize-422): section mode's .mzo object writer.
//
// Decision D-3 reuses src/maize_obj.h's existing layout verbatim rather than minting a second
// object format, because that layout carries no v1 instruction knowledge at all: it is a
// header, a section table, a symbol table, and width-keyed relocation records. Two things are
// added there rather than here, and both are additive: R_MAIZE_REL32 for the
// program-counter-relative targets, and MZO_VERSION_V2 so no consumer can conflate a v1 object
// with a v2 one.
//
// Per D-10 this writes real bytes in this card rather than stubbing the write until a v2-aware
// mzld exists. Nothing consuming the objects yet is the reason maize-434 exists; it is not a
// reason to leave half the assembly language parsed but unproven.

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "../maize_obj.h"
#include "mzasm.h"

namespace maize::v2::asmr {

std::vector<std::uint8_t> Assembler::serialize_object() const {
    using namespace maize::obj;

    // Fixed section order CODE, RODATA, DATA, BSS; only sections that got bytes or space are
    // written, so a module that never opened `data` carries no empty data section.
    const std::uint8_t order[4] = {SEC_CODE, SEC_RODATA, SEC_DATA, SEC_BSS};
    std::vector<std::uint8_t> present;
    int section_index_of[5];
    for (int i = 0; i < 5; ++i) {
        section_index_of[i] = -1;
    }
    for (const std::uint8_t kind : order) {
        if (section_sizes_[kind] > 0) {
            section_index_of[kind] = static_cast<int>(present.size());
            present.push_back(kind);
        }
    }

    // String table: index 0 is the empty string, so a zero name offset means "unnamed".
    std::vector<std::uint8_t> strtab;
    strtab.push_back(0);
    const auto add_string = [&](const std::string& text) {
        const std::uint32_t offset = static_cast<std::uint32_t>(strtab.size());
        for (const char c : text) {
            strtab.push_back(static_cast<std::uint8_t>(c));
        }
        strtab.push_back(0);
        return offset;
    };

    const auto kind_name = [](std::uint8_t kind) -> const char* {
        switch (kind) {
            case SEC_CODE: return ".text";
            case SEC_RODATA: return ".rodata";
            case SEC_DATA: return ".data";
            case SEC_BSS: return ".bss";
            default: return "";
        }
    };

    std::vector<std::uint32_t> section_name_offset;
    for (const std::uint8_t kind : present) {
        section_name_offset.push_back(add_string(kind_name(kind)));
    }

    // Symbols, in name order so the file is deterministic. A `constant` binds a value rather
    // than an address and is not a linkable symbol, so it is not written: that is exactly why
    // assembler.md's Inclusion section exists.
    struct OutSymbol {
        std::uint32_t name_offset = 0;
        std::uint16_t section_index = 0;
        std::uint8_t binding = 0;
        std::uint8_t type = 0;
        std::uint64_t value = 0;
        std::uint64_t size = 0;
    };
    std::vector<OutSymbol> out_symbols;
    std::map<std::string, std::uint32_t> symbol_index;

    for (const auto& [name, symbol] : symbols_) {
        if (symbol.is_constant) {
            continue;
        }
        OutSymbol out;
        out.name_offset = add_string(name);
        if (!symbol.defined) {
            out.section_index = SHN_UNDEF;
            out.binding = BIND_GLOBAL;
            out.type = TYPE_NOTYPE;
            out.value = 0;
        } else {
            const int index = section_index_of[symbol.section];
            if (index >= 0) {
                out.section_index = static_cast<std::uint16_t>(index);
                out.value = symbol.value;
                out.type = symbol.section == SEC_CODE ? TYPE_FUNC : TYPE_OBJECT;
            } else {
                out.section_index = SHN_ABS;
                out.value = symbol.value;
                out.type = TYPE_NOTYPE;
            }
            out.binding = symbol.exported ? BIND_GLOBAL : BIND_LOCAL;
        }
        symbol_index[name] = static_cast<std::uint32_t>(out_symbols.size());
        out_symbols.push_back(out);
    }

    // Relocations, grouped by the section that carries the patched field.
    struct OutRelocation {
        std::uint64_t offset = 0;
        std::uint32_t symbol = 0;
        std::uint8_t type = 0;
        std::int64_t addend = 0;
    };
    std::vector<std::vector<OutRelocation>> section_relocations(present.size());
    for (const Relocation& relocation : relocations_) {
        const int index = section_index_of[relocation.section];
        if (index < 0) {
            continue;
        }
        OutRelocation out;
        out.offset = relocation.offset;
        const auto it = symbol_index.find(relocation.symbol);
        out.symbol = it == symbol_index.end() ? 0 : it->second;
        out.type = relocation.type;
        out.addend = relocation.addend;
        section_relocations[static_cast<std::size_t>(index)].push_back(out);
    }

    const auto entry_symbol = symbol_index.find("_start");
    const std::uint32_t entry = entry_symbol == symbol_index.end() ? ENTRY_NONE
                                                                   : entry_symbol->second;

    // Lay the file out before writing any of it, so every offset in the header is known when
    // the header is serialized.
    std::uint64_t cursor = MZO_HEADER_SIZE;
    const std::uint64_t section_header_offset = cursor;
    cursor += static_cast<std::uint64_t>(present.size()) * SECTION_HDR_SIZE;

    std::vector<std::uint64_t> section_file_offset(present.size(), 0);
    for (std::size_t i = 0; i < present.size(); ++i) {
        if (present[i] != SEC_BSS) {
            section_file_offset[i] = cursor;
            cursor += section_sizes_[present[i]];
        }
    }

    std::vector<std::uint64_t> relocation_offset(present.size(), 0);
    for (std::size_t i = 0; i < present.size(); ++i) {
        if (!section_relocations[i].empty()) {
            relocation_offset[i] = cursor;
            cursor += section_relocations[i].size() * RELOC_SIZE;
        }
    }

    const std::uint64_t symbol_offset = cursor;
    cursor += out_symbols.size() * SYMBOL_SIZE;
    const std::uint64_t string_offset = cursor;

    std::vector<std::uint8_t> file;

    // Header, 48 bytes. The version byte is what tells a v2 object from a v1 one.
    put_u8(file, MZO_MAGIC0);
    put_u8(file, MZO_MAGIC1);
    put_u8(file, MZO_MAGIC2);
    put_u8(file, MZO_VERSION_V2);
    put_u16(file, 0);  // flags
    put_u16(file, static_cast<std::uint16_t>(present.size()));
    put_u64(file, section_header_offset);
    put_u64(file, symbol_offset);
    put_u32(file, static_cast<std::uint32_t>(out_symbols.size()));
    put_u64(file, string_offset);
    put_u32(file, static_cast<std::uint32_t>(strtab.size()));
    put_u32(file, entry);
    put_u32(file, 0);  // reserved

    // Section headers, 40 bytes each.
    for (std::size_t i = 0; i < present.size(); ++i) {
        const std::uint8_t kind = present[i];
        put_u32(file, section_name_offset[i]);
        put_u8(file, kind);
        put_u8(file, default_attrs(kind));
        put_u8(file, 0);  // align, in log2; the assembler emits no section alignment of its own
        put_u8(file, 0);  // reserved
        put_u64(file, kind == SEC_BSS ? 0 : section_file_offset[i]);
        put_u64(file, section_sizes_[kind]);
        put_u64(file, relocation_offset[i]);
        put_u64(file, static_cast<std::uint64_t>(section_relocations[i].size()));
    }

    // Section contents. A bss section holds no emitted bytes, so it contributes none here even
    // though its size is recorded above.
    for (const std::uint8_t kind : present) {
        if (kind == SEC_BSS) {
            continue;
        }
        std::vector<std::uint8_t> bytes = sections_[kind];
        bytes.resize(static_cast<std::size_t>(section_sizes_[kind]), 0);
        file.insert(file.end(), bytes.begin(), bytes.end());
    }

    // Relocation arrays, 24 bytes each.
    for (std::size_t i = 0; i < present.size(); ++i) {
        for (const OutRelocation& relocation : section_relocations[i]) {
            put_u64(file, relocation.offset);
            put_u32(file, relocation.symbol);
            put_u8(file, relocation.type);
            put_u8(file, 0);
            put_u8(file, 0);
            put_u8(file, 0);
            put_u64(file, static_cast<std::uint64_t>(relocation.addend));
        }
    }

    // Symbol table, 24 bytes each.
    for (const OutSymbol& symbol : out_symbols) {
        put_u32(file, symbol.name_offset);
        put_u16(file, symbol.section_index);
        put_u8(file, symbol.binding);
        put_u8(file, symbol.type);
        put_u64(file, symbol.value);
        put_u64(file, symbol.size);
    }

    file.insert(file.end(), strtab.begin(), strtab.end());
    return file;
}

}  // namespace maize::v2::asmr
