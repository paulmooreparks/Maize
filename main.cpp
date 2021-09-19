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
	/* This is a sample program that I load into memory before starting the main loop. Eventually,
	I'll load a binary file with BIOS, OS image, etc. */

	/* This is the address where the CPU starts executing code. */
	maize::word address {0x0000000000001000};

	std::vector<maize::byte> mem {
		/* LD B.B0 0x41         */	cpu::instr::ld_immVal_reg, 0x00, 0x10, 0x41,
		/* LD B.B1 0x0A         */	cpu::instr::ld_immVal_reg, 0x00, 0x11, 0x0A,
		/* OUT B 0x7F           */	cpu::instr::out_regVal_imm, 0x1E, 0x00, 0x7F,
		/* ADD B.B0 B.B1        */	cpu::instr::add_regVal_reg, 0x10, 0x11,
		/* LD B.B1 C.Q0         */	cpu::instr::ld_regVal_reg, 0x11, 0x28,
		/* LD 0x00002000 A.H0   */	cpu::instr::ld_immVal_reg, 0x02, 0x0C, 0x00, 0x20, 0x00, 0x00,
		/* ST B @A.H0           */	cpu::instr::st_regVal_regAddr, 0x1E, 0x0C,
		/* LD @A.H0 D */			cpu::instr::ld_regAddr_reg, 0x0C, 0x3E,
		/* INC B.B1             */	cpu::instr::inc_regVal, 0x11,
		/* LD @0x00002000 E.H0  */	cpu::instr::ld_immAddr_reg, 0x02, 0x4C, 0x00, 0x10, 0x00, 0x00,
		/* HALT                 */	cpu::instr::halt
	};

	/* Load the program above into memory. */
	for (auto& b : mem) {
		cpu::mm.write(address, b);
		++address;
	}

	sys::win::console con;
	con.open();
	cpu::add_device(0x7F, con);
	cpu::run();
	con.close();
}

