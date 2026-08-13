// mzasm_conformance.cpp (maize-422): AC-10 and AC-14.
//
// AC-10 walks kOpcodeTable and mnemonic_v2.h against each other. It proves bijection and
// select/shape family consistency, and it is necessary but NOT sufficient for D-4's anti-drift
// claim: it cannot tell a mnemonic wired to the wrong same-shaped byte from a correct one,
// because `subtract` carrying `multiply`'s opcode is Shape::OpRRR either way and both tables
// would agree on the wrong answer.
//
// AC-14 supplies the missing oracle by parsing appendix-a-opcode-map.md, which is the frozen
// normative assignment rather than anything this project transcribed, and checking all three
// artifacts against it. Read AC-10 as the cheap consistency check and AC-14 as the criterion
// D-4's claim actually rests on.

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "appendix_a.h"
#include "mnemonic_v2.h"
#include "mzasm_test_support.h"
#include "opcode_v2.h"

namespace maize::v2::test {

namespace {

std::string appendix_path() {
    return repo_root() + "/docs/spec-v2/appendix-a-opcode-map.md";
}

const Appendix& appendix() {
    static const Appendix parsed = parse_appendix(appendix_path());
    return parsed;
}

std::string byte_name(std::uint8_t byte) {
    std::ostringstream out;
    out << "$" << std::hex << std::uppercase << (byte < 16 ? "0" : "")
        << static_cast<unsigned>(byte);
    return out.str();
}

std::vector<std::string> split_words(const std::string& text) {
    std::vector<std::string> words;
    std::istringstream in(text);
    std::string word;
    while (in >> word) {
        words.push_back(word);
    }
    return words;
}

// The Form column names a length class from the instruction-encoding chapter, and this is the
// whole of that chapter's list. A form the appendix uses that is not here fails rather than
// mapping to None, because a silent None would compare equal to a reserved byte's shape.
bool shape_from_form(const std::string& form, Shape& out) {
    static const std::map<std::string, Shape> table = {
        {"op", Shape::Op},
        {"op r", Shape::OpR},
        {"op r r", Shape::OpRR},
        {"op r r r", Shape::OpRRR},
        {"op r r r r", Shape::OpRRRR},
        {"op i1", Shape::OpI1},
        {"op i4", Shape::OpI4},
        {"op r i1", Shape::OpRI1},
        {"op r i2", Shape::OpRI2},
        {"op r i4", Shape::OpRI4},
        {"op r i8", Shape::OpRI8},
        {"op r r i1", Shape::OpRRI1},
        {"op r r i2", Shape::OpRRI2},
        {"op r r i4", Shape::OpRRI4},
        {"op r r i1 i1", Shape::OpRRI1I1},
    };
    const auto it = table.find(form);
    if (it == table.end()) {
        return false;
    }
    out = it->second;
    return true;
}

bool slot_from_name(const std::string& name, Slot& out) {
    if (name == "plain") { out = Slot::Plain; return true; }
    if (name == "byte-sliced") { out = Slot::ByteSliced; return true; }
    if (name == "quarter-sliced") { out = Slot::QuarterSliced; return true; }
    if (name == "half-sliced") { out = Slot::HalfSliced; return true; }
    return false;
}

std::vector<std::string> split_slots(const std::string& cell) {
    std::vector<std::string> names;
    std::string current;
    for (const char c : cell) {
        if (c == ',') {
            names.push_back(current);
            current.clear();
        } else if (c != ' ') {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        names.push_back(current);
    }
    return names;
}

const char* select_name(Select select) {
    switch (select) {
        case Select::None: return "None";
        case Select::RegForm: return "RegForm";
        case Select::ImmForm: return "ImmForm";
        case Select::RegTarget: return "RegTarget";
        case Select::DispTarget: return "DispTarget";
        case Select::Bare: return "Bare";
        case Select::Displaced: return "Displaced";
    }
    return "?";
}

// Derive each assigned byte's operand-selection bucket from the appendix's own Operands column,
// which is what determines it. A mnemonic text is NOT unique on its own (`add` appears at $10
// as `add rs1 rs2 rd` and at $31 as `add rs $imm rd`), so rows are keyed on byte and the text is
// only the grouping key.
//
// The classification is applied strictly in this order, and group size comes first: a branch row
// carries a `target` operand but its mnemonic occupies exactly one row, so reading the operand
// spelling before counting the rows would wrongly make every branch a DispTarget.
std::map<std::uint8_t, Select> derive_selects(const Appendix& parsed,
                                              std::vector<std::string>& errors) {
    std::map<std::string, std::vector<std::uint8_t>> groups;
    for (unsigned b = 0; b < 256; ++b) {
        const AppendixRow& row = parsed.bytes[b];
        if (row.disposition == Disposition::Assigned) {
            groups[row.mnemonic].push_back(static_cast<std::uint8_t>(b));
        }
    }

    std::map<std::uint8_t, Select> result;
    for (const auto& [text, bytes] : groups) {
        if (bytes.size() == 1) {
            result[bytes[0]] = Select::None;
            continue;
        }
        if (bytes.size() != 2) {
            errors.push_back("'" + text + "' occupies " + std::to_string(bytes.size()) +
                             " rows, and only one or two have a selection rule");
            continue;
        }
        const std::string& first = parsed.bytes[bytes[0]].operands;
        const std::string& second = parsed.bytes[bytes[1]].operands;

        const auto displaced = [](const std::string& operands) {
            return operands.find("@rb+") != std::string::npos;
        };
        const auto memory = [](const std::string& operands) {
            return operands.find("@rb") != std::string::npos;
        };
        const auto target = [](const std::string& operands) {
            return operands.find("target") != std::string::npos;
        };
        const auto immediate = [](const std::string& operands) {
            return operands.find("$imm") != std::string::npos ||
                   operands.find("#imm") != std::string::npos;
        };

        if (memory(first) || memory(second)) {
            result[bytes[0]] = displaced(first) ? Select::Displaced : Select::Bare;
            result[bytes[1]] = displaced(second) ? Select::Displaced : Select::Bare;
            continue;
        }
        if (target(first) || target(second)) {
            result[bytes[0]] = target(first) ? Select::DispTarget : Select::RegTarget;
            result[bytes[1]] = target(second) ? Select::DispTarget : Select::RegTarget;
            continue;
        }
        // Two rows, no memory operand and no target. `sys` divides the way jump and call do (one
        // operand each, an immediate against a register); every other pair is an ALU or compare
        // sibling written with three operands.
        const bool one_operand_each =
            split_words(first).size() == 2 && split_words(second).size() == 2;
        if (one_operand_each) {
            result[bytes[0]] = immediate(first) ? Select::DispTarget : Select::RegTarget;
            result[bytes[1]] = immediate(second) ? Select::DispTarget : Select::RegTarget;
            continue;
        }
        result[bytes[0]] = immediate(first) ? Select::ImmForm : Select::RegForm;
        result[bytes[1]] = immediate(second) ? Select::ImmForm : Select::RegForm;
    }
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// AC-14, totality
// ---------------------------------------------------------------------------------------

MZ_FIXTURE(appendix_a_parse_is_total) {
    const Appendix& parsed = appendix();

    for (const std::string& error : parsed.errors) {
        record_failure("appendix parse: " + error);
    }

    // Every one of the 256 bytes is accounted for exactly once. The claim() path already
    // reported any double claim as an error above, so what remains to check here is that
    // nothing was skipped: a parser that could not match a row would leave its bytes
    // Unaccounted and fail here rather than passing on the subset it did understand.
    for (unsigned b = 0; b < 256; ++b) {
        if (parsed.bytes[b].disposition == Disposition::Unaccounted) {
            record_failure("byte " + byte_name(static_cast<std::uint8_t>(b)) +
                           " is accounted for by no table, no prose statement and no declaration");
        }
    }

    MZ_CHECK_EQ(static_cast<std::uint64_t>(parsed.count(Disposition::Assigned)), 187u);
    MZ_CHECK_EQ(static_cast<std::uint64_t>(parsed.count(Disposition::Escape)), 7u);
    MZ_CHECK_EQ(static_cast<std::uint64_t>(parsed.count(Disposition::Reserved)), 62u);

    // A.15 states the reserved set a second time, in prose. Comparing the two byte for byte is
    // what catches a reserved row dropped from either place.
    //
    // One byte is deliberately outside that comparison. $00 is stated only in A.2's prose and in
    // A.15's enumeration, never in a band table, so A.15 is what claims it rather than what
    // corroborates it; its independent second statement is A.1's band-summary row, which the
    // band reconciliation below checks. Comparing it here would be comparing A.15 against
    // itself.
    std::set<std::uint8_t> from_tables;
    for (unsigned b = 0; b < 256; ++b) {
        const AppendixRow& row = parsed.bytes[b];
        if (row.disposition == Disposition::Reserved &&
            row.claimed_by.rfind("A.15", 0) != 0) {
            from_tables.insert(static_cast<std::uint8_t>(b));
        }
    }
    const std::set<std::uint8_t> from_prose(parsed.prose_reserved.begin(),
                                            parsed.prose_reserved.end());
    for (const std::uint8_t byte : from_tables) {
        if (from_prose.count(byte) == 0) {
            record_failure("byte " + byte_name(byte) +
                           " is reserved in a band table but missing from A.15's prose set");
        }
    }
    for (const std::uint8_t byte : from_prose) {
        if (parsed.bytes[byte].disposition != Disposition::Reserved) {
            record_failure("byte " + byte_name(byte) +
                           " is in A.15's prose reserved set and is not reserved");
        }
    }
    // The one byte A.15 claims outright rather than corroborates, named here so that a future
    // edit adding a table row for it makes this line wrong and visible rather than silently
    // shifting which statement is load-bearing.
    MZ_CHECK_EQ(static_cast<std::uint64_t>(from_tables.size()), 61u);
    MZ_CHECK_EQ(static_cast<std::uint64_t>(parsed.prose_reserved_stated_count), 62u);
}

// ---------------------------------------------------------------------------------------
// AC-14 (a): mnemonic_v2.h row identity
// ---------------------------------------------------------------------------------------

MZ_FIXTURE(mnemonic_table_matches_appendix) {
    const Appendix& parsed = appendix();
    std::vector<std::string> errors;
    const std::map<std::uint8_t, Select> selects = derive_selects(parsed, errors);
    for (const std::string& error : errors) {
        record_failure("select derivation: " + error);
    }

    std::set<std::uint8_t> seen;
    for (const MnemonicEntry& entry : kMnemonics) {
        const AppendixRow& row = parsed.bytes[entry.opcode];
        const std::string where = "mnemonic_v2.h row for " + byte_name(entry.opcode);

        if (row.disposition != Disposition::Assigned) {
            record_failure(where + " names a byte the appendix does not assign");
            continue;
        }
        if (!seen.insert(entry.opcode).second) {
            record_failure(where + " is a second row for the same byte");
        }
        // This is the check AC-10 cannot make: a same-shaped mnemonic wired to the wrong byte,
        // subtract's row carrying multiply's opcode, is caught here because the appendix says
        // which text belongs to which byte and it is not a table this project wrote.
        if (row.mnemonic != entry.text) {
            record_failure(where + " spells the mnemonic '" + std::string(entry.text) +
                           "', and the appendix assigns that byte to '" + row.mnemonic + "'");
        }
        const auto select = selects.find(entry.opcode);
        if (select == selects.end()) {
            record_failure(where + " has no derived operand-selected form");
        } else if (select->second != entry.select) {
            record_failure(where + " declares select " + select_name(entry.select) +
                           ", and the appendix's Operands spelling '" + row.operands +
                           "' implies " + select_name(select->second));
        }
    }

    // Every assigned byte has a row, so nothing the appendix assigns is unreachable from source.
    for (unsigned b = 0; b < 256; ++b) {
        if (parsed.bytes[b].disposition == Disposition::Assigned && seen.count(static_cast<std::uint8_t>(b)) == 0) {
            record_failure("the appendix assigns " + byte_name(static_cast<std::uint8_t>(b)) +
                           " to '" + parsed.bytes[b].mnemonic +
                           "', and mnemonic_v2.h has no row for it");
        }
    }
}

// ---------------------------------------------------------------------------------------
// AC-14 (b): kOpcodeTable shape, length, kind and slots
// ---------------------------------------------------------------------------------------

MZ_FIXTURE(opcode_table_matches_appendix) {
    const Appendix& parsed = appendix();

    for (unsigned b = 0; b < 256; ++b) {
        const std::uint8_t byte = static_cast<std::uint8_t>(b);
        const AppendixRow& row = parsed.bytes[byte];
        const OpcodeInfo& info = kOpcodeTable[byte];
        const std::string where = "kOpcodeTable[" + byte_name(byte) + "]";

        switch (row.disposition) {
            case Disposition::Reserved:
                if (info.kind != OpcodeKind::Reserved) {
                    record_failure(where + " is not Reserved, and the appendix reserves it");
                }
                continue;
            case Disposition::Escape:
                if (info.kind != OpcodeKind::Escape) {
                    record_failure(where + " is not Escape, and A.13 makes it an escape byte");
                }
                continue;
            case Disposition::Unaccounted:
                continue;  // the totality fixture reports this
            case Disposition::Assigned:
                break;
        }

        if (info.kind != OpcodeKind::Assigned) {
            record_failure(where + " is not Assigned, and the appendix assigns it to '" +
                           row.mnemonic + "'");
            continue;
        }
        Shape expected = Shape::None;
        if (!shape_from_form(row.form, expected)) {
            record_failure("the appendix gives " + byte_name(byte) + " the form '" + row.form +
                           "', which is not one of the fifteen length classes");
            continue;
        }
        if (info.shape != expected) {
            record_failure(where + " has a shape the appendix's form '" + row.form +
                           "' does not imply");
        }
        MZ_CHECK_EQ(info.length, row.length);

        // A.9's Slots column is data the appendix states nowhere else, so those twelve bytes'
        // slot classes are checked here too rather than taken on trust.
        if (!row.slots.empty()) {
            const std::vector<std::string> names = split_slots(row.slots);
            const ShapeInfo shape = shape_info(info.shape);
            if (names.size() != shape.operands) {
                record_failure("the appendix gives " + byte_name(byte) + " " +
                               std::to_string(names.size()) + " slot classes, and its shape has " +
                               std::to_string(shape.operands) + " operand bytes");
                continue;
            }
            for (std::size_t i = 0; i < names.size(); ++i) {
                Slot slot = Slot::None;
                if (!slot_from_name(names[i], slot)) {
                    record_failure("the appendix names slot class '" + names[i] + "' for " +
                                   byte_name(byte) + ", which is not a slot class");
                    continue;
                }
                if (info.slots[i] != slot) {
                    record_failure(where + " slot " + std::to_string(i) +
                                   " disagrees with the appendix's '" + names[i] + "'");
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------
// AC-14 (c): A.1's band summary against the rows actually parsed
// ---------------------------------------------------------------------------------------

MZ_FIXTURE(band_summary_matches_rows) {
    const Appendix& parsed = appendix();
    MZ_CHECK(!parsed.bands.empty());

    int total_assigned = 0;
    int total_escapes = 0;
    int total_reserved = 0;

    for (const BandSummary& band : parsed.bands) {
        int assigned = 0;
        int escapes = 0;
        int reserved = 0;
        for (unsigned b = band.first; b <= band.last; ++b) {
            switch (parsed.bytes[b].disposition) {
                case Disposition::Assigned: ++assigned; break;
                case Disposition::Escape: ++escapes; break;
                case Disposition::Reserved: ++reserved; break;
                case Disposition::Unaccounted: break;
            }
        }
        const std::string where = "A.1 band " + band.range + " (" + band.family + ")";
        if (assigned != band.assigned) {
            record_failure(where + " states " + std::to_string(band.assigned) +
                           " assigned, and its rows give " + std::to_string(assigned));
        }
        if (escapes != band.escapes) {
            record_failure(where + " states " + std::to_string(band.escapes) +
                           " escapes, and its rows give " + std::to_string(escapes));
        }
        if (reserved != band.reserved) {
            record_failure(where + " states " + std::to_string(band.reserved) +
                           " reserved, and its rows give " + std::to_string(reserved));
        }
        total_assigned += band.assigned;
        total_escapes += band.escapes;
        total_reserved += band.reserved;
    }

    // The bands together cover all 256 bytes, so the summary's own totals are the appendix's
    // second statement of the three counts and are checked as such.
    MZ_CHECK_EQ(static_cast<std::uint64_t>(total_assigned), 187u);
    MZ_CHECK_EQ(static_cast<std::uint64_t>(total_escapes), 7u);
    MZ_CHECK_EQ(static_cast<std::uint64_t>(total_reserved), 62u);
}

// ---------------------------------------------------------------------------------------
// The shipped CSR declarations against the chapter that defines them
// ---------------------------------------------------------------------------------------

// asm/v2/csr.mzasm is eighteen hand-transcribed (name, number) pairs, and AC-12 exercises three
// of them. Three of eighteen is not coverage, and a transcription nothing checks is the drift
// class this whole card exists to eliminate; leaving it unguarded while shipping AC-14 next door
// would be inconsistent. privileged-architecture.md's CSR table is an ordinary markdown table
// with a Number and a Name column, so the same technique the appendix parser uses closes it,
// and the shared reader means there is one copy of the cell handling rather than two.
//
// This checks BOTH directions. A register the chapter defines and the file omits is as much a
// defect as a constant the file invents, and a one-directional check would pass a file that had
// quietly lost half its rows.
MZ_FIXTURE(csr_include_matches_the_privileged_architecture_table) {
    std::vector<std::string> errors;
    const std::vector<MarkdownTable> tables =
        read_markdown_tables(repo_root() + "/docs/spec-v2/privileged-architecture.md", errors);
    for (const std::string& error : errors) {
        record_failure("privileged-architecture.md: " + error);
    }

    // The chapter states the CSR table is the complete list of control and status registers in
    // the base, and it is the only table in the chapter carrying a Number and a Name column.
    // Selecting it by its columns rather than by its position means a table added above it does
    // not silently become the one we read.
    std::map<std::string, std::uint64_t> from_chapter;
    int matching_tables = 0;
    for (const MarkdownTable& table : tables) {
        if (!table.has("Number") || !table.has("Name")) {
            continue;
        }
        ++matching_tables;
        for (std::size_t row = 0; row < table.rows.size(); ++row) {
            const std::string number_cell = table.cell(row, "Number");
            const std::string name = table.cell(row, "Name");
            std::uint64_t number = 0;
            if (!parse_hex_number(number_cell, number)) {
                record_failure("the CSR table row for '" + name + "' has number cell '" +
                               number_cell + "', which is not a hexadecimal literal");
                continue;
            }
            if (!from_chapter.emplace(name, number).second) {
                record_failure("the CSR table names '" + name + "' twice");
            }
        }
    }
    // A selector that matched nothing would leave both sets empty and the comparison below
    // vacuous, so the floor is asserted rather than assumed.
    MZ_CHECK_EQ(static_cast<std::uint64_t>(matching_tables), 1u);
    MZ_CHECK_EQ(from_chapter.size(), 18u);

    // The shipped file, parsed as the assembler reads it: `constant <name> <$number>`.
    std::string source;
    if (!read_file_text(repo_root() + "/asm/v2/csr.mzasm", source)) {
        record_failure("cannot read asm/v2/csr.mzasm");
        return;
    }
    std::map<std::string, std::uint64_t> from_file;
    std::istringstream lines(source);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t comment = line.find(';');
        if (comment != std::string::npos) {
            line = line.substr(0, comment);
        }
        std::istringstream fields(line);
        std::string directive;
        std::string name;
        std::string value;
        if (!(fields >> directive >> name >> value) || directive != "constant") {
            continue;
        }
        std::uint64_t number = 0;
        if (!parse_hex_number(value, number)) {
            record_failure("csr.mzasm binds '" + name + "' to '" + value +
                           "', which is not a hexadecimal literal");
            continue;
        }
        if (!from_file.emplace(name, number).second) {
            record_failure("csr.mzasm defines '" + name + "' twice");
        }
    }

    for (const auto& [name, number] : from_chapter) {
        const auto found = from_file.find(name);
        if (found == from_file.end()) {
            record_failure("privileged-architecture.md defines the register '" + name +
                           "' and asm/v2/csr.mzasm does not");
            continue;
        }
        if (found->second != number) {
            std::ostringstream message;
            message << "csr.mzasm binds '" << name << "' to $" << std::hex << std::uppercase
                    << found->second << " and the chapter's table gives it $" << number;
            record_failure(message.str());
        }
    }
    for (const auto& [name, number] : from_file) {
        (void)number;
        if (from_chapter.count(name) == 0) {
            record_failure("asm/v2/csr.mzasm defines '" + name +
                           "', which privileged-architecture.md's CSR table does not");
        }
    }
}

// ---------------------------------------------------------------------------------------
// AC-10: the two hand-written tables against each other
// ---------------------------------------------------------------------------------------

MZ_FIXTURE(mnemonic_and_opcode_tables_agree) {
    std::array<int, 256> rows{};
    for (const MnemonicEntry& entry : kMnemonics) {
        ++rows[entry.opcode];
        if (kOpcodeTable[entry.opcode].kind != OpcodeKind::Assigned) {
            record_failure("mnemonic_v2.h names " + byte_name(entry.opcode) +
                           ", which kOpcodeTable does not assign");
        }
    }
    for (unsigned b = 0; b < 256; ++b) {
        const bool assigned = kOpcodeTable[b].kind == OpcodeKind::Assigned;
        if (assigned && rows[b] != 1) {
            record_failure("kOpcodeTable assigns " + byte_name(static_cast<std::uint8_t>(b)) +
                           " and mnemonic_v2.h has " + std::to_string(rows[b]) + " rows for it");
        }
        if (!assigned && rows[b] != 0) {
            record_failure("kOpcodeTable does not assign " +
                           byte_name(static_cast<std::uint8_t>(b)) +
                           " and mnemonic_v2.h has a row for it");
        }
    }

    // No row's declared operand-selection tag may imply a shape other than the one kOpcodeTable
    // records for its byte. A RegForm row whose opcode carries an immediate is a contradiction,
    // and so is an ImmForm row whose opcode carries none.
    for (const MnemonicEntry& entry : kMnemonics) {
        const ShapeInfo shape = shape_info(kOpcodeTable[entry.opcode].shape);
        const std::string where =
            "mnemonic_v2.h row for " + byte_name(entry.opcode) + " ('" + entry.text + "')";
        switch (entry.select) {
            case Select::RegForm:
            case Select::RegTarget:
                if (shape.immediates != 0) {
                    record_failure(where + " is " + select_name(entry.select) +
                                   ", and its opcode carries an immediate");
                }
                break;
            case Select::ImmForm:
            case Select::DispTarget:
                if (shape.immediates == 0) {
                    record_failure(where + " is " + select_name(entry.select) +
                                   ", and its opcode carries no immediate");
                }
                break;
            case Select::Bare:
                if (shape.immediates != 0) {
                    record_failure(where + " is Bare, and its opcode carries a displacement");
                }
                break;
            case Select::Displaced:
                if (shape.immediates != 1) {
                    record_failure(where + " is Displaced, and its opcode carries no displacement");
                }
                break;
            case Select::None:
                break;
        }
    }
}

}  // namespace maize::v2::test
