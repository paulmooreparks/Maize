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

int main(int argc, char *argv[]) {
	using namespace cpu;

	if (argc < 2) {
		std::cerr << "Missing path to binary" << std::endl;
	}

	std::string file_path {argv[1]};
	std::ifstream fin(file_path, std::fstream::binary);

	/* Slurp the whole image so the loader can inspect the magic before deciding
	   how to lay it out. */
	std::vector<char> buf((std::istreambuf_iterator<char>(fin)),
		std::istreambuf_iterator<char>());
	fin.close();

	/* Additive .mzx branch (maize-12). Any non-.mzx image falls through to the
	   legacy flat load-at-0 path, byte for byte, preserving flat .bin support. */
	if (!load_mzx(buf)) {
		maize::u_word address {0x0000000000000000};
		for (char c : buf) {
			cpu::mm.write_byte(address, static_cast<u_byte>(c));
			++address;
		}
	}

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
}
