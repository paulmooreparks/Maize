/* This isn't classic OOP. My primary concern is performance; readability is secondary. Readability 
and maintainability are still important, though, because I want this to become something 
that I can use to write any CPU implementation. */

#include <vector>
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

int main() {
	using namespace cpu;

	/* This is the address where the CPU starts executing code. */
	maize::word code_address {0x0000000000001000};

	/* I'm parking some test data further down in memory. */
	maize::word text_address {0x0000000000002000};

	/* This is a sample program that I load into memory before starting the main loop. Eventually,
	I'll load a binary file with BIOS, OS image, etc. */

	std::vector<maize::byte> code {
#if false
		/* LD 0x41 B.B0         */	cpu::instr::ld_immVal_reg, 0x00, 0x10, 0x41,
		/* LD 0x0A B.B1         */	cpu::instr::ld_immVal_reg, 0x00, 0x11, 0x0A,
		/* OUT B 0x7F           */	cpu::instr::out_regVal_imm, 0x1E, 0x00, 0x7F,
		/* ADD B.B0 B.B1        */	cpu::instr::add_regVal_reg, 0x10, 0x11,
		/* LD B.B1 C.Q0         */	cpu::instr::ld_regVal_reg, 0x11, 0x28,
		/* LD 0x00002000 A.H0   */	cpu::instr::ld_immVal_reg, 0x02, 0x0C, 0x00, 0x20, 0x00, 0x00,
		/* ST B @A.H0           */	cpu::instr::st_regVal_regAddr, 0x1E, 0x0C,
		/* LD @A.H0 D */			cpu::instr::ld_regAddr_reg, 0x0C, 0x3E,
		/* INC B.B1             */	cpu::instr::inc_regVal, 0x11,
		/* LD @0x00002000 E.H0  */	cpu::instr::ld_immAddr_reg, 0x02, 0x4C, 0x00, 0x10, 0x00, 0x00,
		/* HALT                 */	cpu::instr::halt
#endif
		/* LD $00001000 SP.H0 */	cpu::instr::ld_immVal_reg, 0x02, 0xFC, 0x00, 0x10, 0x00, 0x00,
		/* LD SP.H0 SP.H1 */		cpu::instr::ld_regVal_reg, 0xFC, 0XFD,
		/* PUSH $1234 */			cpu::instr::push_immVal, 0x01, 0x12, 0x34,
		/* POP A.Q0 */				cpu::instr::pop_regVal, 0x08,
		/* PUSH A.Q0 */				cpu::instr::push_regVal, 0x08,
		/* POP B.B0 */				cpu::instr::pop_regVal, 0x18, 
		/* LD $01 G */				cpu::instr::ld_immVal_reg, 0x00, 0x50, 0x01,
		/* LD 0x00002000 H.H0 */	cpu::instr::ld_immVal_reg, 0x02, 0x6C, 0x00, 0x20, 0x00, 0x00,
		/* LD 0x0D J */				cpu::instr::ld_immVal_reg, 0x00, 0x70, 0x0D,
		/* SYS $01 */				cpu::instr::sys_immVal, 0x00, 0x01,
		/* HALT */					cpu::instr::halt,
	};

	std::vector<maize::byte> text {
		'H', 'e', 'l', 'l', 'o', ',', ' ', 'w', 'o', 'r', 'l', 'd', '!'
	};

	/* Load the program above into memory. */
	for (auto& b : code) {
		cpu::mm.write(code_address, b);
		++code_address;
	}

	for (auto& b : text) {
		cpu::mm.write(text_address, b);
		++text_address;
	}

	sys::init();
	cpu::run();
	sys::exit();
}

