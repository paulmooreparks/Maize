#include <filesystem>
#include <vector>
#include <iostream>
#include <fstream>
#include "maize.h"

#include "maize.h"
using namespace maize;

int main(int argc, char *argv[]) {
	using namespace cpu;

	if (argc < 2) {
		std::cerr << "Missing path to binary" << std::endl;
	}

	std::string file_path {argv[1]};
	std::ifstream fin(file_path, std::fstream::binary);

	maize::u_word address {0x0000000000000000};
	char c {0};

	while (fin.read(&c, 1)) {
		cpu::mm.write_byte(address, static_cast<u_byte>(c));
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

