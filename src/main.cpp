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
	sys::init();
	cpu::run();
#ifdef __linux__ 
	std::cout << std::endl;
#endif
	sys::exit();
}

