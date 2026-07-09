#include <filesystem>
#include <vector>
#include <iostream>
#include <fstream>
#include <cstdint>
#include "maize.h"
#include "maize_obj.h"

#include "maize.h"
using namespace maize;

/* maize-12: load a linked .mzx executable. Walk the segment table, copy each
   segment's file bytes to its load address, zero-fill NOBITS/uninitialized
   tails, and set PC to the recorded entry. Returns false if the buffer is not a
   .mzx image so the caller can fall back to the legacy flat load. */
static bool load_mzx(const std::vector<char> &buf) {
	using namespace maize::obj;

	if (buf.size() < MZX_HEADER_SIZE) {
		return false;
	}
	const std::uint8_t *b = reinterpret_cast<const std::uint8_t *>(buf.data());
	if (b[0] != MZX_MAGIC0 || b[1] != MZX_MAGIC1 || b[2] != MZX_MAGIC2 || b[3] != MZX_VERSION) {
		return false;
	}

	std::uint16_t seg_count = get_u16(b, 6);
	std::uint64_t entry     = get_u64(b, 8);
	std::uint64_t shoff     = get_u64(b, 16);

	for (std::uint16_t i = 0; i < seg_count; ++i) {
		std::size_t so = static_cast<std::size_t>(shoff) + i * SEGMENT_SIZE;
		if (so + SEGMENT_SIZE > buf.size()) {
			std::cerr << "maize: malformed .mzx (segment table out of bounds)" << std::endl;
			return true;
		}
		std::uint64_t vaddr     = get_u64(b, so + 8);
		std::uint64_t file_off  = get_u64(b, so + 16);
		std::uint64_t mem_size  = get_u64(b, so + 24);
		std::uint64_t file_size = get_u64(b, so + 32);

		if (file_off + file_size > buf.size()) {
			std::cerr << "maize: malformed .mzx (segment contents out of bounds)" << std::endl;
			return true;
		}

		/* Copy the present bytes to the load address. */
		for (std::uint64_t j = 0; j < file_size; ++j) {
			cpu::mm.write_byte(vaddr + j, static_cast<u_byte>(b[file_off + j]));
		}
		/* Zero-fill the NOBITS / uninitialized tail (BSS, or mem_size > file_size). */
		for (std::uint64_t j = file_size; j < mem_size; ++j) {
			cpu::mm.write_byte(vaddr + j, static_cast<u_byte>(0));
		}
	}

	cpu::regs::rp.w0 = entry;
	return true;
}

static void print_usage(std::ostream &out) {
	out <<
		"maize - run a Maize program\n"
		"\n"
		"Usage:\n"
		"  maize <program>      run a compiled Maize program (.mzb or .mzx)\n"
		"  maize -h, --help     show this help\n"
		"\n"
		"Example:\n"
		"  maize hello.mzb\n"
		"\n"
		"Passing arguments to the program is not supported yet.\n";
}

int main(int argc, char *argv[]) {
	using namespace cpu;

	/* card maize-67: well-behaved-interpreter hardening for direct execution. As
	   an OS handler (Linux binfmt_misc / Windows file association) maize is
	   invoked as `maize <image-path> [args...]`, so the image path is argv[1].
	   Guard the missing-argument case (previously it printed an error but then
	   dereferenced argv[1] anyway) and fail closed with a clear message if the
	   image cannot be opened. Extra arguments (argv[2]+) are tolerated and
	   ignored; delivering them to the guest is out of scope (maize-60). An
	   absolute path works unchanged (std::ifstream accepts it verbatim). */
	if (argc >= 2 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
		print_usage(std::cout);
		return 0;
	}

	/* card maize-68: a bare invocation or an unrecognized leading-'-' flag are
	   both CLI-usage errors -- maize exposes no flags besides -h/--help today,
	   so a leading '-' is never a legitimate image path in practice. Print the
	   same full help block used for -h/--help rather than a one-line string,
	   matching the mzld/mazm convention, and fail closed (exit 2) before ever
	   touching the filesystem. */
	if (argc < 2 || argv[1][0] == '-') {
		print_usage(std::cerr);
		return 2;
	}

	std::string file_path {argv[1]};
	std::ifstream fin(file_path, std::fstream::binary);
	if (!fin.is_open()) {
		std::cerr << "maize: cannot open image '" << file_path << "'" << std::endl;
		return 2;
	}

	/* Slurp the whole image so the loader can inspect the magic before deciding
	   how to lay it out. */
	std::vector<char> buf((std::istreambuf_iterator<char>(fin)),
		std::istreambuf_iterator<char>());
	fin.close();

	/* Additive .mzx branch (maize-12). Any non-.mzx image falls through to the
	   legacy flat load-at-0 path, byte for byte, preserving flat .mzb support. */
	if (!load_mzx(buf)) {
		maize::u_word address {0x0000000000000000};
		for (char c : buf) {
			cpu::mm.write_byte(address, static_cast<u_byte>(c));
			++address;
		}
	}

	/* card maize-57: process-start stack-pointer contract. Set RS explicitly to
	   the highest 8-byte-aligned address in the flat 64-bit space, for both the
	   .mzx and flat-.mzb load paths, so the initial SP is an intentional line
	   rather than the implicit zero-init of a process-global register. The stack
	   is full-descending: the first push pre-decrements RS and lands at
	   0xFFFFFFFFFFFFFFF0. No wraparound is relied upon. RP is already set per
	   path (load_mzx records the .mzx entry; a flat image leaves RP at 0). */
	regs::rs.w0 = 0xFFFFFFFFFFFFFFF8;

	/* card maize-10, Decision D6465: a single always-registered loopback test device,
	   solely so OUT/OUTR/IN have a real port to exercise in regression tests. Bare
	   `device` instance with no specialized behavior (src/maize_cpu.h class device).
	   Port 1 is the fixed test port; asm/test_outr_in.mazm targets it directly. */
	cpu::device loopback_test_device;
	cpu::add_device(1, loopback_test_device);

	sys::init();
	cpu::run();
#ifdef __linux__
	std::cout << std::endl;
#endif
	sys::exit();

	/* card maize-58: surface main's return value as the host process's own exit
	   status. SYS $3C (sys_exit) recorded the low 8 bits of R0 here; a program
	   that ended via HALT recorded nothing, so this defaults to 0. The host's
	   normal 8-bit process-status truncation applies on top. */
	return sys::exit_code();
}
