#include <filesystem>
#include <vector>
#include <iostream>
#include <fstream>
#include <cstdint>
#include <string>
#include <cctype>
#include "maize.h"
#include "maize_obj.h"

#include "maize.h"
using namespace maize;

/* maize-12: load a linked .mzx executable. Walk the segment table, copy each
   segment's file bytes to its load address, zero-fill NOBITS/uninitialized
   tails, and set PC to the recorded entry. Returns false if the buffer is not a
   .mzx image so the caller can fall back to the legacy flat load. */
static bool load_mzx(const std::vector<char> &buf, u_word &image_end) {
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

		/* maize-75: track the image high-water mark so the heap base can sit
		   just past the last loaded byte across every segment. */
		u_word seg_end = static_cast<u_word>(vaddr + mem_size);
		if (seg_end > image_end) {
			image_end = seg_end;
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
		"  maize [options] <image> [guest-args...]\n"
		"\n"
		"Options are consumed up to the first non-option token, which is the image.\n"
		"Everything after <image> is passed to the program as its argv, verbatim.\n"
		"argv[0] is <image> exactly as you typed it.\n"
		"\n"
		"Options:\n"
		"  -h, --help                 show this help\n"
		"  -e, --env KEY=VAL          add one environment variable (repeatable)\n"
		"      --env=KEY=VAL          same, inline form\n"
		"      --env-file <path>      add variables from a KEY=VAL file (repeatable;\n"
		"                             blank lines and #-comment lines are ignored)\n"
		"  --                         end options; the next token is <image>\n"
		"\n"
		"The program's environment is built only from -e/--env and --env-file; the\n"
		"host's own environment is never inherited.\n"
		"\n"
		"Example:\n"
		"  maize --env GREETING=hi hello.mzb alpha beta\n";
}

/* maize-60: a guest environment KEY must match [A-Za-z_][A-Za-z0-9_]*. */
static bool valid_env_key(const std::string &key) {
	if (key.empty()) {
		return false;
	}
	unsigned char c0 = static_cast<unsigned char>(key[0]);
	if (!(std::isalpha(c0) || c0 == '_')) {
		return false;
	}
	for (char ch : key) {
		unsigned char c = static_cast<unsigned char>(ch);
		if (!(std::isalnum(c) || c == '_')) {
			return false;
		}
	}
	return true;
}

/* maize-60: validate and append one "KEY=VAL" environment entry. Split on the FIRST
   '=' only, so VAL may itself contain '=' or be empty. The stored string is the raw
   "KEY=VAL" (the value is the verbatim remainder, no quoting/expansion). Returns
   false (after printing a stderr diagnostic) on a malformed entry -- the caller then
   fails closed with exit 2. */
static bool add_env_entry(const std::string &entry, std::vector<std::string> &env) {
	std::string::size_type eq = entry.find('=');
	if (eq == std::string::npos) {
		std::cerr << "maize: malformed environment entry '" << entry
			<< "' (expected KEY=VAL)" << std::endl;
		return false;
	}
	std::string key = entry.substr(0, eq);
	if (!valid_env_key(key)) {
		std::cerr << "maize: invalid environment key '" << key
			<< "' (must match [A-Za-z_][A-Za-z0-9_]*)" << std::endl;
		return false;
	}
	env.push_back(entry);
	return true;
}

/* maize-60: load KEY=VAL lines from an --env-file. Blank lines and lines whose first
   non-whitespace character is '#' are ignored; every other line is validated exactly
   like an inline -e value. Leading whitespace before the key is trimmed; no shell
   quoting or $-expansion. Returns false (stderr diagnostic) if the file cannot be
   opened or any line is malformed. */
static bool load_env_file(const std::string &path, std::vector<std::string> &env) {
	std::ifstream f(path);
	if (!f.is_open()) {
		std::cerr << "maize: cannot open env-file '" << path << "'" << std::endl;
		return false;
	}
	std::string line;
	while (std::getline(f, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();   /* tolerate CRLF checkouts */
		}
		std::string::size_type start = line.find_first_not_of(" \t");
		if (start == std::string::npos) {
			continue;          /* blank line */
		}
		if (line[start] == '#') {
			continue;          /* comment line */
		}
		if (!add_env_entry(line.substr(start), env)) {
			return false;
		}
	}
	return true;
}

/* maize-60: build the System V-style process-start block at the top of RAM and point
   RS at its base (&argc). Layout (low -> high address), all 8-byte-aligned:

       [RS+0]   argc
       [RS+8]   argv[0..argc-1], then a NULL terminator word
       [...]    envp[0..envc-1], then a NULL terminator word
       [strings, higher in memory, each NUL-terminated, ending at top-of-RAM]

   Pointer/argc words are written with mm.write_word (64-bit little-endian) so they
   read back through LD @; string bytes with mm.write_byte. This amends maize-57: RS
   no longer starts empty at 0xFFFFFFFFFFFFFFF8; that value is now TOP-of-RAM, and RS
   points at argc. The stack stays full-descending -- the first guest PUSH
   pre-decrements RS into the free region BELOW the block. */
static void build_process_start_block(const std::vector<std::string> &guest_argv,
	const std::vector<std::string> &guest_envp) {
	using namespace cpu;

	const u_word TOP = 0xFFFFFFFFFFFFFFF8ull;
	const std::size_t argc_n = guest_argv.size();
	const std::size_t envc_n = guest_envp.size();

	/* 1. Pack the NUL-terminated strings DOWN from TOP, in emission order
	      (argv then envp), recording each entry's absolute guest address. */
	std::size_t blob_len = 0;
	for (const std::string &s : guest_argv) blob_len += s.size() + 1;
	for (const std::string &s : guest_envp) blob_len += s.size() + 1;

	const u_word string_base = TOP - static_cast<u_word>(blob_len);

	std::vector<u_word> argv_addr(argc_n);
	std::vector<u_word> envp_addr(envc_n);

	u_word cur = string_base;
	for (std::size_t k = 0; k < argc_n; ++k) {
		argv_addr[k] = cur;
		for (char ch : guest_argv[k]) {
			mm.write_byte(cur++, static_cast<u_byte>(ch));
		}
		mm.write_byte(cur++, static_cast<u_byte>(0));
	}
	for (std::size_t k = 0; k < envc_n; ++k) {
		envp_addr[k] = cur;
		for (char ch : guest_envp[k]) {
			mm.write_byte(cur++, static_cast<u_byte>(ch));
		}
		mm.write_byte(cur++, static_cast<u_byte>(0));
	}

	/* 2. Pointer block = argc + (argv + NULL) + (envp + NULL). */
	const u_word ptr_block_size =
		static_cast<u_word>(8 * (1 + (argc_n + 1) + (envc_n + 1)));

	/* 3. RS = (string_base - ptr_block_size) rounded DOWN to an 8-byte boundary.
	      Rounding down only grows the gap to the strings; it never overlaps. */
	const u_word rs = (string_base - ptr_block_size) & ~static_cast<u_word>(7);

	/* 4. Write argc, argv[] + NULL, envp[] + NULL from RS upward. */
	u_word p = rs;
	mm.write_word(p, static_cast<u_word>(argc_n));   p += 8;
	for (std::size_t k = 0; k < argc_n; ++k) { mm.write_word(p, argv_addr[k]); p += 8; }
	mm.write_word(p, static_cast<u_word>(0));         p += 8;   /* argv NULL */
	for (std::size_t k = 0; k < envc_n; ++k) { mm.write_word(p, envp_addr[k]); p += 8; }
	mm.write_word(p, static_cast<u_word>(0));         p += 8;   /* envp NULL */

	regs::rs.w0 = rs;
}

int main(int argc, char *argv[]) {
	using namespace cpu;

	/* card maize-60: CLI grammar `maize [options] <image> [guest-args...]`. Parse
	   maize-options left-to-right and STOP at the first non-option token, which is
	   <image>; a `--` also ends option parsing. Everything after <image> is guest
	   argv[1..], verbatim, even if it starts with '-' (a guest -e is NOT a maize
	   flag). argv[0] is <image> exactly as invoked. This preserves maize-67's
	   missing-arg / unreadable-image guards and maize-68's full-help-on-usage-error
	   behavior. The guest environment is built ONLY from -e/--env/--env-file below;
	   the host's ambient environment is never inherited. */
	std::vector<std::string> env_entries;
	int idx = 1;
	while (idx < argc) {
		std::string arg {argv[idx]};

		if (arg == "-h" || arg == "--help") {
			print_usage(std::cout);
			return 0;
		}
		if (arg == "--") {
			++idx;                 /* next token is <image>, whatever it looks like */
			break;
		}
		if (arg == "-e" || arg == "--env") {
			if (idx + 1 >= argc) {
				std::cerr << "maize: option '" << arg
					<< "' requires a KEY=VAL argument" << std::endl;
				print_usage(std::cerr);
				return 2;
			}
			if (!add_env_entry(argv[idx + 1], env_entries)) {
				return 2;
			}
			idx += 2;
			continue;
		}
		if (arg.rfind("--env=", 0) == 0) {
			if (!add_env_entry(arg.substr(6), env_entries)) {
				return 2;
			}
			++idx;
			continue;
		}
		if (arg == "--env-file") {
			if (idx + 1 >= argc) {
				std::cerr << "maize: option '--env-file' requires a <path> argument"
					<< std::endl;
				print_usage(std::cerr);
				return 2;
			}
			if (!load_env_file(argv[idx + 1], env_entries)) {
				return 2;
			}
			idx += 2;
			continue;
		}
		if (arg.rfind("--env-file=", 0) == 0) {
			if (!load_env_file(arg.substr(11), env_entries)) {
				return 2;
			}
			++idx;
			continue;
		}
		if (!arg.empty() && arg[0] == '-') {
			/* Unrecognized leading-'-' flag before <image>: usage error. */
			std::cerr << "maize: unknown option '" << arg << "'" << std::endl;
			print_usage(std::cerr);
			return 2;
		}
		/* First non-option token: this is <image>. Stop parsing options. */
		break;
	}

	/* No image token (bare invocation, or `--`/options with nothing after them). */
	if (idx >= argc) {
		print_usage(std::cerr);
		return 2;
	}

	/* Guest argv: argv[0] = <image> as invoked; argv[1..] = the post-image tokens
	   verbatim. The launcher/crt0 must NOT basename or rewrite argv[0]. */
	std::string file_path {argv[idx]};
	std::vector<std::string> guest_argv;
	guest_argv.push_back(file_path);
	for (int k = idx + 1; k < argc; ++k) {
		guest_argv.push_back(std::string(argv[k]));
	}

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
	maize::u_word image_end {0};
	if (!load_mzx(buf, image_end)) {
		maize::u_word address {0x0000000000000000};
		for (char c : buf) {
			cpu::mm.write_byte(address, static_cast<u_byte>(c));
			++address;
		}
		image_end = static_cast<maize::u_word>(buf.size());
	}

	/* card maize-60 (amends maize-57): process-start contract. Build a System V-style
	   process-start block at the top of RAM -- argc, then argv[] + NULL, then envp[] +
	   NULL, with the NUL-terminated argument/environment strings packed at higher
	   addresses ending at TOP (0xFFFFFFFFFFFFFFF8) -- and set RS to its 8-byte-aligned
	   base so [RS] == argc. This replaces maize-57's bare `RS = 0xFFFFFFFFFFFFFFF8`
	   empty-initial-stack assignment; that value is now TOP-of-RAM for block
	   placement. The stack stays full-descending: the first guest PUSH pre-decrements
	   RS into the free region BELOW the block. crt0 marshals argc/argv/envp off this
	   block into R0/R1/R2 before calling main. The other reset guarantees
	   (guaranteed-zero registers, no wraparound, RP set per load path -- load_mzx
	   records the .mzx entry, a flat image leaves RP at 0) are unchanged. */
	/* maize-75: seed the brk heap floor (align_up(image_end,16)) from the loaded
	   image's high-water mark, BEFORE the process-start block is built near TOP.
	   SYS $0C moves the break above this floor; the two regions never meet. */
	sys::init_heap(image_end);

	build_process_start_block(guest_argv, env_entries);

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
