/* This isn't classic OOP. My primary concern is performance; readability is secondary. Readability 
and maintainability are still important, though, because I want this to become something 
that I can use to write any CPU implementation. */

#include <filesystem>
#include <vector>
#include <iostream>
#include <fstream>
#include "maize.h"

/* Right now, the code is still a bit scattered and not up to my usual standards, but I'll
clean it up soon. This is not intended to be pure OO code; my priorities are speed and
super-tight generated assembly. */

/* This program doesn't really "do" anything yet, at least not as visible output. So far it's
just a platform for testing instructions as I implement them. There's some code implemented as
machine language in the vector in the main function (at the bottom of this file) that I load
into memory before starting the main loop. You'll need to run this in the debugger to watch
the data move around as it executes. */

#include "maize.h"
using namespace maize;

int main(int argc, char *argv[]) {
	using namespace cpu;

#if false
	/* This is the address where the CPU starts executing code. */
	maize::word code_address {0x0000000000001000};

	/* I'm parking some test data further down in memory. */
	maize::word text_address {0x0000000000002000};

	/* This is a sample program that I load into memory before starting the main loop. Eventually,
	I'll load a binary file with BIOS, OS image, etc. */

	std::vector<maize::byte> code {
		/* $00001000: LD $00003000 SP.H0 */		cpu::instr::ld_immVal_reg, 0x02, 0xFC, 0x00, 0x30, 0x00, 0x00,
		/* $00001007: LD SP.H0 SP.H1 */			cpu::instr::ld_regVal_reg, 0xFC, 0XFD,
		/* $0000100A: PUSH $1234 */				cpu::instr::push_immVal, 0x01, 0x34, 0x12,
		/* $0000100E: POP A.Q0 */				cpu::instr::pop_regVal, 0x08,
		/* $00001010: PUSH A.Q0 */				cpu::instr::push_regVal, 0x08,
		/* $00001012: POP B.B0 */				cpu::instr::pop_regVal, 0x10, 
		/* $00001014: LD $01 G */				cpu::instr::ld_immVal_reg, 0x00, 0x5E, 0x01,
		/* $00001018: LD $00002000 H.H0 */		cpu::instr::ld_immVal_reg, 0x02, 0x6C, 0x00, 0x20, 0x00, 0x00,
		/* $0000101F: LD $0D J */				cpu::instr::ld_immVal_reg, 0x00, 0x7E, 0x0D,
		/* $00001023: SYS $01 */				cpu::instr::sys_immVal, 0x00, 0x01,
	    /* $00001026: LD $00 G */				cpu::instr::ld_immVal_reg, 0x00, 0x5E, 0x00,
		/* $0000102A: LD $00002100 H.H0 */		cpu::instr::ld_immVal_reg, 0x02, 0x6C, 0x00, 0x21, 0x00, 0x00,
		/* $00001031: LD $01 J */				cpu::instr::ld_immVal_reg, 0x00, 0x7E, 0x01,
		/* $00001035: SYS $00 */				cpu::instr::sys_immVal, 0x00, 0x00,
		/* $00001038: LD $01 G */				cpu::instr::ld_immVal_reg, 0x00, 0x5E, 0x01,
		/* $0000103C: LD $00002100 H.H0 */		cpu::instr::ld_immVal_reg, 0x02, 0x6C, 0x00, 0x21, 0x00, 0x00,
		/* $00001043: LD $01 J */				cpu::instr::ld_immVal_reg, 0x00, 0x7E, 0x01,
		/* $00001047: SYS $01 */				cpu::instr::sys_immVal, 0x00, 0x01,
		/* $0000104A: HALT */					cpu::instr::halt_opcode,
	};

	std::vector<maize::byte> text {
		'H', 'e', 'l', 'l', 'o', ',', ' ', 'w', 'o', 'r', 'l', 'd', '!'
	};

	/* Load the program above into memory. */
	for (auto &b : code) {
		cpu::mm.write(code_address, b);
		++code_address;
	}

	for (auto &b : text) {
		cpu::mm.write(text_address, b);
		++text_address;
	}
#endif

#if true
	if (argc < 2) {
		std::cerr << "Missing path to binary" << std::endl;
	}

	std::string file_path {argv[1]};
	std::ifstream fin(file_path, std::fstream::binary);

	maize::word address {0x0000000000000000};
	char c {0};

	while (fin.read(&c, 1)) {
		cpu::mm.write(address, static_cast<byte>(c));
		++address;
	}

	fin.close();
#endif

	sys::init();
	cpu::run();
#ifdef __linux__ 
	std::cout << std::endl;
#endif
	sys::exit();
}

