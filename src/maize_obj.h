#pragma once
/* maize-12: shared on-disk layout for the Maize relocatable object format
   (.mzo) and the linked executable format (.mzx).

   This header is the single source of truth for the byte layouts documented in
   README ("Object format" / "Executable format"). mazm (object-emission mode),
   mzld (the linker), and maize (the loader) all include it so the producer and
   the consumers cannot drift.

   All multi-byte fields are little-endian, matching the ISA's immediate encoding
   and the flat-64 memory model. Records are serialized field-by-field through the
   put_* / get_* helpers below rather than by memcpy of packed structs, so host
   struct padding never leaks into the file. */

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace maize {
namespace obj {

	/* ---- .mzo (relocatable object) --------------------------------------- */

	constexpr std::uint8_t  MZO_MAGIC0 = 'M';
	constexpr std::uint8_t  MZO_MAGIC1 = 'Z';
	constexpr std::uint8_t  MZO_MAGIC2 = 'O';
	constexpr std::uint8_t  MZO_VERSION = 0x01;

	constexpr std::size_t   MZO_HEADER_SIZE  = 48;
	constexpr std::size_t   SECTION_HDR_SIZE = 40;
	constexpr std::size_t   SYMBOL_SIZE      = 24;
	constexpr std::size_t   RELOC_SIZE       = 24;

	/* ---- .mza (prebuilt runtime archive, maize-302) ---------------------- */

	/* An ar-style container holding each runtime .mzo member verbatim plus a
	   member index, with its own magic distinct from MZO. It is an internal,
	   replaceable implementation choice (design build-performance.md section 5,
	   the project DIRT ruling): the stability contract is the .mzo object format
	   plus the ABI, NOT this container, so the container may change later (for
	   dynamic linking) with no ABI impact. Written by mzcc's runtime-archive
	   builder; read by mzld, which sniffs MZA_MAGIC to tell an archive input from
	   a plain .mzo and expands the archive's members in declared order.

	   v1 (maize-302) links the WHOLE archive (member selection is deferred to
	   maize-306), so the format carries only what a whole-archive link needs: a
	   member index over verbatim .mzo blobs. A global-symbol -> member map is
	   deliberately NOT stored here in v1; it would be data no consumer reads
	   until member selection exists. maize-306 adds it with the selection pass.

	   Layout (all offsets from file start, little-endian, matching .mzo/.mzx):
	     header (16 bytes): magic 'M','Z','A', version (byte 3); u16 flags (0);
	                        u16 member_count; u64 index_off
	     index  (member_count * 24 bytes, at index_off), per member:
	                        u32 name_off (byte offset into the string table);
	                        u32 reserved (0);
	                        u64 member_off (byte offset of the member .mzo bytes);
	                        u64 member_size (byte length of the member .mzo)
	     strtab (NUL-terminated member tag strings)
	     members (each member's .mzo bytes verbatim, in declared order) */

	constexpr std::uint8_t  MZA_MAGIC0 = 'M';
	constexpr std::uint8_t  MZA_MAGIC1 = 'Z';
	constexpr std::uint8_t  MZA_MAGIC2 = 'A';
	constexpr std::uint8_t  MZA_VERSION = 0x01;

	constexpr std::size_t   MZA_HEADER_SIZE      = 16;
	constexpr std::size_t   MZA_INDEX_ENTRY_SIZE = 24;

	/* ---- .mzx (linked executable) ---------------------------------------- */

	constexpr std::uint8_t  MZX_MAGIC0 = 'M';
	constexpr std::uint8_t  MZX_MAGIC1 = 'Z';
	constexpr std::uint8_t  MZX_MAGIC2 = 'X';
	constexpr std::uint8_t  MZX_VERSION = 0x01;

	constexpr std::size_t   MZX_HEADER_SIZE  = 24;
	constexpr std::size_t   SEGMENT_SIZE     = 40;

	/* ---- section kinds --------------------------------------------------- */

	enum section_kind : std::uint8_t {
		SEC_NULL   = 0,
		SEC_CODE   = 1,
		SEC_RODATA = 2,
		SEC_DATA   = 3,
		SEC_BSS    = 4,
	};

	/* ---- section attribute bitfield -------------------------------------- */

	constexpr std::uint8_t ATTR_EXEC  = 0x01;
	constexpr std::uint8_t ATTR_READ  = 0x02;
	constexpr std::uint8_t ATTR_WRITE = 0x04;
	constexpr std::uint8_t ATTR_ALLOC = 0x08;
	constexpr std::uint8_t ATTR_NOBITS = 0x10;

	/* Canonical attribute set per section kind (Decision 6428 / AC6). */
	inline std::uint8_t default_attrs(std::uint8_t kind) {
		switch (kind) {
			case SEC_CODE:   return ATTR_EXEC | ATTR_READ | ATTR_ALLOC;
			case SEC_RODATA: return ATTR_READ | ATTR_ALLOC;
			case SEC_DATA:   return ATTR_READ | ATTR_WRITE | ATTR_ALLOC;
			case SEC_BSS:    return ATTR_READ | ATTR_WRITE | ATTR_ALLOC | ATTR_NOBITS;
			default:         return 0;
		}
	}

	/* ---- symbol binding / type ------------------------------------------- */

	enum sym_binding : std::uint8_t {
		BIND_LOCAL  = 0,
		BIND_GLOBAL = 1,
		BIND_WEAK   = 2, /* reserved for a future consumer (OQ4) */
	};

	enum sym_type : std::uint8_t {
		TYPE_NOTYPE  = 0,
		TYPE_FUNC    = 1,
		TYPE_OBJECT  = 2,
		TYPE_SECTION = 3,
	};

	/* Sentinel section indices in a symbol entry. */
	constexpr std::uint16_t SHN_UNDEF = 0xFFFF;
	constexpr std::uint16_t SHN_ABS   = 0xFFF0;

	/* Sentinel entry-symbol index in the object header. */
	constexpr std::uint32_t ENTRY_NONE = 0xFFFFFFFFu;

	/* ---- relocation types (keyed to immediate-operand width) ------------- */

	enum reloc_type : std::uint8_t {
		R_MAIZE_NONE  = 0,
		R_MAIZE_ABS8  = 1,
		R_MAIZE_ABS16 = 2,
		R_MAIZE_ABS32 = 3,
		R_MAIZE_ABS64 = 4,
		/* 5..15 reserved for R_MAIZE_REL* (PC-relative) and future kinds. */
	};

	/* Bytes patched by an absolute relocation type, or 0 if not an ABS type. */
	inline std::size_t reloc_width(std::uint8_t r_type) {
		switch (r_type) {
			case R_MAIZE_ABS8:  return 1;
			case R_MAIZE_ABS16: return 2;
			case R_MAIZE_ABS32: return 4;
			case R_MAIZE_ABS64: return 8;
			default:            return 0;
		}
	}

	/* Absolute relocation type for a patch of the given width in bytes. */
	inline std::uint8_t reloc_for_width(std::size_t width) {
		switch (width) {
			case 1: return R_MAIZE_ABS8;
			case 2: return R_MAIZE_ABS16;
			case 4: return R_MAIZE_ABS32;
			case 8: return R_MAIZE_ABS64;
			default: return R_MAIZE_NONE;
		}
	}

	/* ---- little-endian serialization helpers ----------------------------- */

	inline void put_u8(std::vector<std::uint8_t> &b, std::uint8_t v) {
		b.push_back(v);
	}

	inline void put_u16(std::vector<std::uint8_t> &b, std::uint16_t v) {
		b.push_back(static_cast<std::uint8_t>(v & 0xFF));
		b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
	}

	inline void put_u32(std::vector<std::uint8_t> &b, std::uint32_t v) {
		for (int i = 0; i < 4; ++i) {
			b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
		}
	}

	inline void put_u64(std::vector<std::uint8_t> &b, std::uint64_t v) {
		for (int i = 0; i < 8; ++i) {
			b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
		}
	}

	inline void put_bytes(std::vector<std::uint8_t> &b, const void *src, std::size_t n) {
		const std::uint8_t *p = static_cast<const std::uint8_t *>(src);
		b.insert(b.end(), p, p + n);
	}

	inline void pad_to(std::vector<std::uint8_t> &b, std::size_t n, std::uint8_t fill = 0) {
		while (b.size() < n) {
			b.push_back(fill);
		}
	}

	inline std::uint8_t get_u8(const std::uint8_t *b, std::size_t off) {
		return b[off];
	}

	inline std::uint16_t get_u16(const std::uint8_t *b, std::size_t off) {
		return static_cast<std::uint16_t>(b[off])
			| (static_cast<std::uint16_t>(b[off + 1]) << 8);
	}

	inline std::uint32_t get_u32(const std::uint8_t *b, std::size_t off) {
		std::uint32_t v = 0;
		for (int i = 0; i < 4; ++i) {
			v |= static_cast<std::uint32_t>(b[off + i]) << (8 * i);
		}
		return v;
	}

	inline std::uint64_t get_u64(const std::uint8_t *b, std::size_t off) {
		std::uint64_t v = 0;
		for (int i = 0; i < 8; ++i) {
			v |= static_cast<std::uint64_t>(b[off + i]) << (8 * i);
		}
		return v;
	}

} /* namespace obj */
} /* namespace maize */
