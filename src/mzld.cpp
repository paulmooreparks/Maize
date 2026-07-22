/* maize-12: mzld, the Maize linker.

   Consumes one or more relocatable objects (.mzo), resolves symbols, lays out
   sections by attribute group in the fixed order CODE, RODATA, DATA, BSS,
   applies width-keyed relocations, runs a hygiene pass, and emits a linked
   executable (.mzx). See README "Object format" and "Executable format" for the
   on-disk byte layouts, which live in src/maize_obj.h.

   Usage:  mzld [-o out.mzx] [-e entry_symbol] in1.mzo [in2.mzo ...]
           default output = a.mzx, default entry symbol = _start
*/

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "maize_obj.h"

using namespace maize::obj;

namespace {

	int fail(const std::string &msg) {
		std::cerr << "mzld: error: " << msg << std::endl;
		return 1;
	}

	struct Reloc {
		std::uint64_t r_offset {0};
		std::uint32_t r_symbol {0};
		std::uint8_t  r_type {0};
		std::int64_t  r_addend {0};
	};

	struct Symbol {
		std::string   name;
		std::uint16_t section_index {SHN_UNDEF};
		std::uint8_t  binding {BIND_LOCAL};
		std::uint8_t  type {TYPE_NOTYPE};
		std::uint64_t value {0};
		std::uint64_t size {0};
	};

	struct Section {
		std::string   name;
		std::uint8_t  kind {SEC_NULL};
		std::uint8_t  attrs {0};
		std::uint8_t  align {1};
		std::uint64_t size {0};
		std::vector<std::uint8_t> data;   /* empty for NOBITS */
		std::vector<Reloc> relocs;
		std::uint64_t vaddr {0};           /* assigned at layout */
		bool          placed {false};
	};

	struct Object {
		std::string source;                /* file path, for diagnostics */
		std::vector<Section> sections;
		std::vector<Symbol>  symbols;
	};

	bool read_file(const std::string &path, std::vector<std::uint8_t> &out) {
		std::ifstream in(path, std::ios::binary);
		if (!in) {
			return false;
		}
		in.seekg(0, std::ios::end);
		std::streamoff n = in.tellg();
		in.seekg(0, std::ios::beg);
		out.resize(static_cast<std::size_t>(n));
		if (n > 0) {
			in.read(reinterpret_cast<char *>(out.data()), n);
		}
		return static_cast<bool>(in) || in.eof();
	}

	std::string read_string(const std::vector<std::uint8_t> &strtab, std::uint32_t off) {
		if (off >= strtab.size()) {
			return std::string();
		}
		const char *base = reinterpret_cast<const char *>(strtab.data());
		return std::string(base + off);
	}

	/* Parse a .mzo image already resident in `buf` (from a file, or a member
	   sliced out of a .mza archive) into an Object. `path` names the source for
	   diagnostics. Returns "" on success, else an error msg. */
	std::string parse_object_buf(const std::vector<std::uint8_t> &buf,
			const std::string &path, Object &obj) {
		if (buf.size() < MZO_HEADER_SIZE) {
			return "'" + path + "' is too small to be a .mzo";
		}
		const std::uint8_t *b = buf.data();
		if (b[0] != MZO_MAGIC0 || b[1] != MZO_MAGIC1 || b[2] != MZO_MAGIC2 || b[3] != MZO_VERSION) {
			return "'" + path + "' is not a v1 .mzo object (bad magic)";
		}

		std::uint16_t section_count = get_u16(b, 6);
		std::uint64_t shoff        = get_u64(b, 8);
		std::uint64_t symoff       = get_u64(b, 16);
		std::uint32_t symcount     = get_u32(b, 24);
		std::uint64_t stroff       = get_u64(b, 28);
		std::uint32_t strsize      = get_u32(b, 36);

		if (stroff + strsize > buf.size()) {
			return "'" + path + "' string table out of bounds";
		}
		std::vector<std::uint8_t> strtab(b + stroff, b + stroff + strsize);

		obj.source = path;

		/* Sections. */
		for (std::uint16_t i = 0; i < section_count; ++i) {
			std::size_t so = static_cast<std::size_t>(shoff) + i * SECTION_HDR_SIZE;
			if (so + SECTION_HDR_SIZE > buf.size()) {
				return "'" + path + "' section header out of bounds";
			}
			Section s;
			std::uint32_t name_off = get_u32(b, so + 0);
			s.name  = read_string(strtab, name_off);
			s.kind  = get_u8(b, so + 4);
			s.attrs = get_u8(b, so + 5);
			s.align = get_u8(b, so + 6);
			if (s.align == 0) {
				s.align = 1;
			}
			std::uint64_t file_off    = get_u64(b, so + 8);
			s.size                    = get_u64(b, so + 16);
			std::uint64_t reloc_off   = get_u64(b, so + 24);
			std::uint64_t reloc_count = get_u64(b, so + 32);

			bool nobits = (s.attrs & ATTR_NOBITS) != 0;
			if (!nobits && s.size > 0) {
				if (file_off + s.size > buf.size()) {
					return "'" + path + "' section '" + s.name + "' contents out of bounds";
				}
				s.data.assign(b + file_off, b + file_off + s.size);
			}

			/* Relocations for this section. reloc_off points directly at the array;
			   reloc_count (section header +32) gives its length (OQ2: stored, not
			   derived, for a self-describing header). */
			if (reloc_off != 0 && reloc_count != 0) {
				if (reloc_off + reloc_count * RELOC_SIZE > buf.size()) {
					return "'" + path + "' section '" + s.name + "' relocations out of bounds";
				}
				for (std::uint64_t r = 0; r < reloc_count; ++r) {
					std::size_t ro = static_cast<std::size_t>(reloc_off)
						+ static_cast<std::size_t>(r) * RELOC_SIZE;
					Reloc rel;
					rel.r_offset = get_u64(b, ro + 0);
					rel.r_symbol = get_u32(b, ro + 8);
					rel.r_type   = get_u8(b, ro + 12);
					rel.r_addend = static_cast<std::int64_t>(get_u64(b, ro + 16));
					s.relocs.push_back(rel);
				}
			}

			obj.sections.push_back(std::move(s));
		}

		/* Symbols. */
		for (std::uint32_t i = 0; i < symcount; ++i) {
			std::size_t yo = static_cast<std::size_t>(symoff) + i * SYMBOL_SIZE;
			if (yo + SYMBOL_SIZE > buf.size()) {
				return "'" + path + "' symbol table out of bounds";
			}
			Symbol sym;
			std::uint32_t name_off = get_u32(b, yo + 0);
			sym.name          = read_string(strtab, name_off);
			sym.section_index = get_u16(b, yo + 4);
			sym.binding       = get_u8(b, yo + 6);
			sym.type          = get_u8(b, yo + 7);
			sym.value         = get_u64(b, yo + 8);
			sym.size          = get_u64(b, yo + 16);
			obj.symbols.push_back(std::move(sym));
		}

		return "";
	}

	/* Expand a .mza runtime archive resident in `buf` into `objects`, appending
	   every member (in the archive's declared order) as its own Object. maize-302
	   v1 links the WHOLE archive (member selection is deferred to maize-306), so
	   every member is pulled here; the classic on-demand fixpoint rides in with
	   that later card. Members land in `objects` in declared order, and because
	   mzcc passes the archive as the FIRST link input, the runtime objects occupy
	   the same leading layout positions they held when compiled from source, so
	   the linked image stays behavior-parity (and, absent member selection,
	   byte-parity) with the from-source build. Returns "" on success. */
	std::string parse_archive(const std::string &path,
			const std::vector<std::uint8_t> &buf, std::vector<Object> &objects) {
		if (buf.size() < MZA_HEADER_SIZE) {
			return "'" + path + "' is too small to be a .mza archive";
		}
		const std::uint8_t *b = buf.data();
		if (b[3] != MZA_VERSION) {
			return "'" + path + "' is not a v1 .mza archive (bad version)";
		}
		std::uint16_t member_count = get_u16(b, 6);
		std::uint64_t index_off    = get_u64(b, 8);
		for (std::uint16_t i = 0; i < member_count; ++i) {
			std::size_t eo = static_cast<std::size_t>(index_off)
				+ static_cast<std::size_t>(i) * MZA_INDEX_ENTRY_SIZE;
			if (eo + MZA_INDEX_ENTRY_SIZE > buf.size()) {
				return "'" + path + "' archive member index out of bounds";
			}
			std::uint32_t name_off    = get_u32(b, eo + 0);
			std::uint64_t member_off  = get_u64(b, eo + 8);
			std::uint64_t member_size = get_u64(b, eo + 16);
			if (member_off + member_size > buf.size()) {
				return "'" + path + "' archive member contents out of bounds";
			}
			std::string tag;
			for (std::size_t k = name_off; k < buf.size() && b[k] != 0; ++k) {
				tag.push_back(static_cast<char>(b[k]));
			}
			std::vector<std::uint8_t> member(
				b + static_cast<std::size_t>(member_off),
				b + static_cast<std::size_t>(member_off + member_size));
			Object obj;
			std::string mname = path + "(" + (tag.empty() ? std::to_string(i) : tag) + ")";
			std::string err = parse_object_buf(member, mname, obj);
			if (!err.empty()) {
				return err;
			}
			objects.push_back(std::move(obj));
		}
		return "";
	}

	std::uint64_t align_up(std::uint64_t v, std::uint64_t a) {
		if (a <= 1) {
			return v;
		}
		return (v + (a - 1)) & ~(a - 1);
	}

} /* anonymous namespace */

static void print_usage(std::ostream &out) {
	out <<
		"usage: mzld [options] <input.mzo> [<input.mzo> ...]\n"
		"\n"
		"Maize linker. Links one or more relocatable .mzo objects into a single\n"
		"linked .mzx executable, resolving symbols and applying relocations. On\n"
		"error no output is produced. An input may also be a .mza runtime archive,\n"
		"whose members are linked in the archive's declared order.\n"
		"\n"
		"options:\n"
		"  -o <out.mzx>   output path for the linked executable (default: a.mzx)\n"
		"  -e <entry>     entry-point symbol name (default: _start)\n"
		"  --map <path>   also write a map file: one '0x<address> <name>' line per\n"
		"                 defined symbol, sorted by address (for profiling tools)\n"
		"  -b <hex>       link base address in hex (default: 2000)\n"
		"  -h, --help     show this help and exit\n"
		"\n"
		"At least one input .mzo object is required.\n";
}

int main(int argc, char *argv[]) {
	std::string out_path = "a.mzx";
	std::string entry_name = "_start";
	std::string map_path;              /* --map <path>: write an address/name sidecar */
	std::vector<std::string> inputs;
	/* card maize-24 (decision D8): the link base defaults to 0x2000 (see the layout
	   comment below), overridable with -b <hex> so a resident image such as quesOS can
	   link at a reserved non-default base clear of the 0x2000 children it execs. */
	std::uint64_t image_base = 0x2000;

	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		if (arg == "-h" || arg == "--help") {
			print_usage(std::cout);
			return 0;
		}
		else if (arg == "-o" && i + 1 < argc) {
			out_path = argv[++i];
		}
		else if (arg == "-e" && i + 1 < argc) {
			entry_name = argv[++i];
		}
		else if (arg == "--map" && i + 1 < argc) {
			map_path = argv[++i];
		}
		else if (arg == "-b" && i + 1 < argc) {
			std::string v = argv[++i];
			std::size_t pos = 0;
			try {
				image_base = std::stoull(v, &pos, 16);
			}
			catch (...) {
				return fail("invalid -b link base '" + v + "' (expected a hex address)");
			}
			if (pos != v.size()) {
				return fail("invalid -b link base '" + v + "' (expected a hex address)");
			}
		}
		else if (!arg.empty() && arg[0] == '-') {
			return fail("unknown flag '" + arg + "'");
		}
		else {
			inputs.push_back(arg);
		}
	}

	if (inputs.empty()) {
		print_usage(std::cerr);
		return 1;
	}

	/* Read each input, sniffing its magic: a .mza runtime archive (maize-302)
	   expands into its member objects in declared order; a plain .mzo parses to
	   one object. Both .mzo and .mza begin 'M','Z'; the third byte ('O' vs 'A')
	   discriminates. An explicitly named .mzo always links; archive members ride
	   in via parse_archive (whole-archive in v1, on-demand selection in
	   maize-306). Downstream layout, symbol resolution, relocation, and the
	   hygiene passes all operate on the resulting `objects` vector unchanged, so
	   the W+X and segment-overlap checks cover archive members too (AC9). */
	std::vector<Object> objects;
	objects.reserve(inputs.size());
	for (const auto &path : inputs) {
		std::vector<std::uint8_t> buf;
		if (!read_file(path, buf)) {
			return fail("cannot read input '" + path + "'");
		}
		if (buf.size() >= 4 && buf[0] == MZA_MAGIC0 && buf[1] == MZA_MAGIC1
				&& buf[2] == MZA_MAGIC2) {
			std::string err = parse_archive(path, buf, objects);
			if (!err.empty()) {
				return fail(err);
			}
		}
		else {
			Object obj;
			std::string err = parse_object_buf(buf, path, obj);
			if (!err.empty()) {
				return fail(err);
			}
			objects.push_back(std::move(obj));
		}
	}

	/* Hygiene: reject any W+X section up front (Decision 6431 / AC6). */
	for (const auto &obj : objects) {
		for (const auto &s : obj.sections) {
			if ((s.attrs & ATTR_EXEC) && (s.attrs & ATTR_WRITE)) {
				return fail("section '" + s.name + "' in '" + obj.source
					+ "' is both writable and executable (W+X)");
			}
		}
	}

	/* Layout: fixed order CODE, RODATA, DATA, BSS. Assign each contributing section a
	   load address, honoring per-section alignment.

	   The base is NOT 0: the CPU's interrupt/trap vector table is a fixed hardware region
	   at [0x1000, 0x1800) (trap_vector_table_base + 256 * 8, src/maize_cpu.h). A program
	   that installs interrupt handlers writes entries there, so its own code/data must not
	   occupy it. Laying out from 0 let any image larger than 4 KB overlap the table, so an
	   interrupt-using C program corrupted itself (and non-interrupt programs only escaped
	   because they never read the table). Start above the table, page-aligned; this also
	   leaves [0, 0x2000) unmapped as a null-pointer guard. The base defaults to 0x2000
	   and is overridable with -b <hex> (decision D8; parsed above). A non-default base
	   is the caller's responsibility to keep clear of the vector table [0x1000, 0x1800)
	   and of any other resident image it will share the flat address space with. */
	const std::uint8_t order[] = { SEC_CODE, SEC_RODATA, SEC_DATA, SEC_BSS };
	std::uint64_t cursor = image_base;
	for (std::uint8_t k : order) {
		for (auto &obj : objects) {
			for (auto &s : obj.sections) {
				if (s.kind != k || s.size == 0) {
					continue;
				}
				cursor = align_up(cursor, s.align);
				s.vaddr = cursor;
				s.placed = true;
				cursor += s.size;
			}
		}
	}

	/* Build the global symbol table (GLOBAL definitions), rejecting duplicates. */
	struct GlobalDef { const Object *obj; std::size_t sym_index; };
	std::map<std::string, GlobalDef> globals;
	for (const auto &obj : objects) {
		for (std::size_t i = 0; i < obj.symbols.size(); ++i) {
			const Symbol &sym = obj.symbols[i];
			if (sym.binding == BIND_GLOBAL && sym.section_index != SHN_UNDEF) {
				auto it = globals.find(sym.name);
				if (it != globals.end()) {
					return fail("duplicate global symbol '" + sym.name + "' defined in '"
						+ it->second.obj->source + "' and '" + obj.source + "'");
				}
				globals[sym.name] = GlobalDef{ &obj, i };
			}
		}
	}

	/* Resolve one symbol (referenced from a given object) to a final address.
	   Returns false and sets err on an undefined symbol. */
	auto resolve = [&](const Object &obj, std::uint32_t sym_index,
			std::uint64_t &out_addr, std::string &err) -> bool {
		if (sym_index >= obj.symbols.size()) {
			err = "relocation references out-of-range symbol index";
			return false;
		}
		const Symbol &sym = obj.symbols[sym_index];
		if (sym.section_index == SHN_ABS) {
			out_addr = sym.value;
			return true;
		}
		if (sym.section_index == SHN_UNDEF) {
			auto it = globals.find(sym.name);
			if (it == globals.end()) {
				err = "undefined symbol '" + sym.name + "' referenced from '" + obj.source + "'";
				return false;
			}
			const Symbol &def = it->second.obj->symbols[it->second.sym_index];
			out_addr = it->second.obj->sections[def.section_index].vaddr + def.value;
			return true;
		}
		/* Defined locally in this object. */
		if (sym.section_index >= obj.sections.size()) {
			err = "symbol '" + sym.name + "' names an out-of-range section";
			return false;
		}
		out_addr = obj.sections[sym.section_index].vaddr + sym.value;
		return true;
	};

	/* Apply relocations. Patches land in the section's own data buffer. */
	for (auto &obj : objects) {
		for (auto &s : obj.sections) {
			for (const auto &rel : s.relocs) {
				std::size_t width = reloc_width(rel.r_type);
				if (width == 0) {
					return fail("unsupported relocation type "
						+ std::to_string((int)rel.r_type) + " in '" + obj.source + "'");
				}
				if (rel.r_offset + width > s.data.size()) {
					return fail("relocation at offset "
						+ std::to_string(rel.r_offset) + " in section '" + s.name
						+ "' of '" + obj.source + "' lands outside the section");
				}
				std::uint64_t sym_addr = 0;
				std::string err;
				if (!resolve(obj, rel.r_symbol, sym_addr, err)) {
					return fail(err);
				}
				std::uint64_t value = sym_addr + static_cast<std::uint64_t>(rel.r_addend);

				/* Range check: value must fit the relocation width. */
				if (width < 8) {
					std::uint64_t limit = (std::uint64_t(1) << (width * 8));
					if (value >= limit) {
						return fail("relocation value 0x" + [&]{
								char tmp[32];
								std::snprintf(tmp, sizeof(tmp), "%llx",
									(unsigned long long)value);
								return std::string(tmp);
							}() + " does not fit in " + std::to_string(width * 8)
							+ "-bit relocation for symbol referenced from '"
							+ obj.source + "'");
					}
				}

				for (std::size_t j = 0; j < width; ++j) {
					s.data[rel.r_offset + j] =
						static_cast<std::uint8_t>((value >> (8 * j)) & 0xFF);
				}
			}
		}
	}

	/* Resolve the entry point. */
	std::uint64_t entry_addr = 0;
	{
		auto it = globals.find(entry_name);
		bool found = false;
		if (it != globals.end()) {
			const Symbol &def = it->second.obj->symbols[it->second.sym_index];
			entry_addr = it->second.obj->sections[def.section_index].vaddr + def.value;
			found = true;
		}
		else {
			/* Fall back to any (even LOCAL) definition of the entry name. */
			for (const auto &obj : objects) {
				for (const auto &sym : obj.symbols) {
					if (sym.name == entry_name && sym.section_index != SHN_UNDEF
							&& sym.section_index < obj.sections.size()) {
						entry_addr = obj.sections[sym.section_index].vaddr + sym.value;
						found = true;
						break;
					}
				}
				if (found) {
					break;
				}
			}
		}
		if (!found) {
			return fail("entry symbol '" + entry_name + "' is unresolved");
		}
	}

	/* --map: write the address/name sidecar for profilers and debuggers. One line per
	   DEFINED symbol at its final linked address, sorted ascending: "0x<addr> <name>".
	   Locals are INCLUDED because static C functions arrive as local symbols carrying
	   their real names, and dropping them makes whole static bodies mis-attribute to
	   the nearest preceding global (a kernel image is almost entirely static
	   functions). What IS excluded is the compiler's generated basic-block jump
	   targets ("Lm" + digits), which carry no attribution value and would shred a
	   profile into per-block rows. */
	if (!map_path.empty()) {
		auto is_block_label = [](const std::string &n) {
			if (n.size() < 3 || n[0] != 'L' || n[1] != 'm') { return false; }
			for (std::size_t k = 2; k < n.size(); ++k) {
				if (n[k] < '0' || n[k] > '9') { return false; }
			}
			return true;
		};
		std::vector<std::pair<std::uint64_t, const std::string*>> map_rows;
		for (const auto &obj : objects) {
			for (const auto &sym : obj.symbols) {
				if (sym.section_index == SHN_UNDEF || sym.section_index >= obj.sections.size()) {
					continue;
				}
				if (sym.name.empty() || is_block_label(sym.name)) {
					continue;
				}
				map_rows.emplace_back(obj.sections[sym.section_index].vaddr + sym.value, &sym.name);
			}
		}
		std::sort(map_rows.begin(), map_rows.end(),
			[](const auto &a, const auto &b) {
				return a.first != b.first ? a.first < b.first : *a.second < *b.second;
			});
		std::ofstream mf(map_path, std::ios::trunc);
		if (!mf) {
			return fail("cannot write map file '" + map_path + "'");
		}
		char buf[32];
		for (const auto &row : map_rows) {
			std::snprintf(buf, sizeof buf, "0x%016llx ",
				static_cast<unsigned long long>(row.first));
			mf << buf << *row.second << "\n";
		}
	}

	/* Merge contributing sections into one segment per kind, in load order.
	   Sections of a kind were laid out contiguously above, so each kind forms a
	   single [vaddr, vaddr+size) span. */
	struct Segment {
		std::uint8_t kind {0};
		std::uint8_t attrs {0};
		std::uint64_t vaddr {0};
		std::uint64_t mem_size {0};
		std::vector<std::uint8_t> data; /* empty for NOBITS */
		bool nobits {false};
		bool used {false};
	};
	Segment segs[4];
	const std::uint8_t seg_kind[4] = { SEC_CODE, SEC_RODATA, SEC_DATA, SEC_BSS };
	for (int si = 0; si < 4; ++si) {
		std::uint8_t k = seg_kind[si];
		Segment &seg = segs[si];
		seg.kind = k;
		seg.attrs = default_attrs(k);
		seg.nobits = (k == SEC_BSS);
		bool first = true;
		for (auto &obj : objects) {
			for (auto &s : obj.sections) {
				if (s.kind != k || !s.placed) {
					continue;
				}
				if (first) {
					seg.vaddr = s.vaddr;
					first = false;
				}
				/* Pad any inter-section alignment gap with zeros so the segment's
				   in-file bytes stay dense and correctly addressed. */
				std::uint64_t seg_end = seg.vaddr + seg.mem_size;
				if (s.vaddr > seg_end) {
					std::uint64_t gap = s.vaddr - seg_end;
					if (!seg.nobits) {
						seg.data.insert(seg.data.end(), gap, 0);
					}
					seg.mem_size += gap;
				}
				if (!seg.nobits) {
					seg.data.insert(seg.data.end(), s.data.begin(), s.data.end());
				}
				seg.mem_size += s.size;
				seg.used = true;
			}
		}
	}

	/* Hygiene: verify no two used segments overlap in address space. */
	for (int a = 0; a < 4; ++a) {
		if (!segs[a].used) {
			continue;
		}
		for (int c = a + 1; c < 4; ++c) {
			if (!segs[c].used) {
				continue;
			}
			std::uint64_t a0 = segs[a].vaddr, a1 = segs[a].vaddr + segs[a].mem_size;
			std::uint64_t c0 = segs[c].vaddr, c1 = segs[c].vaddr + segs[c].mem_size;
			if (a0 < c1 && c0 < a1) {
				return fail("linked segments overlap in address space");
			}
		}
	}

	/* Serialize the .mzx. */
	std::vector<Segment *> used;
	for (int si = 0; si < 4; ++si) {
		if (segs[si].used) {
			used.push_back(&segs[si]);
		}
	}

	std::uint16_t seg_count = static_cast<std::uint16_t>(used.size());
	std::uint64_t table_off = MZX_HEADER_SIZE;
	std::uint64_t contents_off = table_off + static_cast<std::uint64_t>(seg_count) * SEGMENT_SIZE;

	std::vector<std::uint8_t> file;
	/* Header. */
	put_u8(file, MZX_MAGIC0);
	put_u8(file, MZX_MAGIC1);
	put_u8(file, MZX_MAGIC2);
	put_u8(file, MZX_VERSION);
	put_u16(file, 0);                 /* flags */
	put_u16(file, seg_count);
	put_u64(file, entry_addr);
	put_u64(file, table_off);         /* shoff */

	/* Segment table. Assign file offsets for non-NOBITS segments. */
	std::uint64_t cur_off = contents_off;
	std::vector<std::uint64_t> seg_file_off(used.size(), 0);
	for (std::size_t i = 0; i < used.size(); ++i) {
		Segment *seg = used[i];
		std::uint64_t file_off = 0;
		std::uint64_t file_size = 0;
		if (!seg->nobits) {
			file_off = cur_off;
			file_size = seg->data.size();
			cur_off += file_size;
		}
		seg_file_off[i] = file_off;

		put_u8(file, seg->kind);
		put_u8(file, seg->attrs);
		for (int p = 0; p < 6; ++p) {
			put_u8(file, 0);          /* reserved */
		}
		put_u64(file, seg->vaddr);
		put_u64(file, file_off);
		put_u64(file, seg->mem_size);
		put_u64(file, file_size);
	}

	/* Segment contents. */
	for (std::size_t i = 0; i < used.size(); ++i) {
		Segment *seg = used[i];
		if (!seg->nobits) {
			file.insert(file.end(), seg->data.begin(), seg->data.end());
		}
	}

	std::ofstream fout(out_path, std::ios::binary);
	if (!fout) {
		return fail("cannot open output '" + out_path + "'");
	}
	fout.write(reinterpret_cast<const char *>(file.data()), file.size());
	fout.close();
	if (!fout) {
		return fail("failed writing '" + out_path + "'");
	}

	std::cout << "Linked " << objects.size() << " object(s) -> " << out_path
		<< " (entry 0x" << std::hex << entry_addr << std::dec << ")" << std::endl;
	return 0;
}
