#include <filesystem>
#include <vector>
#include <iostream>
#include <fstream>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <cctype>
#include <chrono>
#include "maize.h"
#include "maize_obj.h"
#include "hostfs/hostfs_core.h"
#include "devices.h"
#include "perf.h"
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

/* maize-225: is the process stdin an interactive terminal? Used to decide whether maizec's
   `input=keyboard` neutralization should fire: only for an interactive tty (where the stdin
   scancode injector would block forever, the maize-217 hang), NOT for a pipe or file that is
   deliberately feeding scancodes (a keyboard test, or an intentional redirect). */
static bool stdin_is_interactive() {
#ifdef _WIN32
	return _isatty(0) != 0;
#else
	return isatty(0) != 0;
#endif
}

#include "maize.h"
using namespace maize;

/* Parse a `<width>x<height>` resolution spec (e.g. "320x200") into positive 16-bit
   dimensions. Returns false on a malformed or zero/overflowing value so the caller can
   fail closed with a diagnostic. */
static bool parse_resolution(const std::string &spec, maize::u_hword &width, maize::u_hword &height) {
	std::string::size_type x = spec.find_first_of("xX");
	if (x == std::string::npos || x == 0 || x + 1 >= spec.size()) {
		return false;
	}
	std::string w_str = spec.substr(0, x);
	std::string h_str = spec.substr(x + 1);
	for (char c : w_str) { if (!std::isdigit(static_cast<unsigned char>(c))) { return false; } }
	for (char c : h_str) { if (!std::isdigit(static_cast<unsigned char>(c))) { return false; } }
	unsigned long w = 0;
	unsigned long h = 0;
	try {
		w = std::stoul(w_str);
		h = std::stoul(h_str);
	} catch (...) {
		return false;
	}
	if (w == 0 || h == 0 || w > 0xFFFF || h > 0xFFFF) {
		return false;
	}
	width = static_cast<maize::u_hword>(w);
	height = static_cast<maize::u_hword>(h);
	return true;
}

/* maize-220: the text console renders each cell as one glyph of the built-in bitmap
   font. These MUST match FONT_W/FONT_H in src/font8x16.h; the console pixel canvas is
   simply (cols * cell width) x (rows * cell height). */
static const maize::u_hword CONSOLE_CELL_W = 8;
static const maize::u_hword CONSOLE_CELL_H = 16;

/* maize-220: parse a `<cols>x<rows>` console-size spec (e.g. "132x43") into a pixel
   canvas, reusing parse_resolution's WxH validation and multiplying by the font cell
   size. Clamps the cell geometry to a sane range (cols 20..500, rows 10..200) so a
   typo cannot request a multi-megapixel console. Returns false on malformed or
   out-of-range input so the caller can warn and keep the default. */
static bool parse_console_size(const std::string &spec, maize::u_hword &width, maize::u_hword &height) {
	maize::u_hword cols = 0;
	maize::u_hword rows = 0;
	if (!parse_resolution(spec, cols, rows)) {
		return false;
	}
	if (cols < 20 || cols > 500 || rows < 10 || rows > 200) {
		return false;
	}
	width = static_cast<maize::u_hword>(cols * CONSOLE_CELL_W);
	height = static_cast<maize::u_hword>(rows * CONSOLE_CELL_H);
	return true;
}

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
		"There are two VM binaries: 'maizec' is the console build (its output goes to the\n"
		"terminal; use it for programs that print, e.g. hello, quesOS), and 'maize' is the\n"
		"graphical build (an on-screen window; use it for the framebuffer, e.g. DOOM). Both\n"
		"take the same options below. On 'maize', --pause-on-halt holds the window open on\n"
		"the final frame (and any --show-perf report) until you press a key or close it.\n"
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
		"      --mount HOST=/GUEST[:ro|:rw]  grant the guest a *nix view of a host\n"
		"                             directory (repeatable; read-only unless :rw)\n"
		"      --mount-home[=HOST]    map the host home to /home/user, read-write\n"
		"      --root <hostpath>      use this host dir as the sandbox root instead\n"
		"                             of the default ~/.maize/root\n"
		"      --no-root              disable the sandbox root; the guest starts with\n"
		"                             an empty filesystem (only explicit --mount grants)\n"
		"  --                         end options; the next token is <image>\n"
		"\n"
		"The program's environment is built only from -e/--env, --env-file, and the\n"
		"optional standing default file ~/.maize/env (loaded first, then overridden by\n"
		"any -e/--env/--env-file entry); the host's own environment is never inherited.\n"
		"\n"
		"Two optional operator files under ~/.maize (HOME/USERPROFILE) tune startup:\n"
		"  ~/.maize/config   default values for the launcher flags, one key=value per\n"
		"                    line (blank and #-comment lines ignored). Keys are the long\n"
		"                    flag names without dashes: display-scale, refresh-hz,\n"
		"                    resolution, console-size, root, show-perf, pause-on-halt,\n"
		"                    display, input, no-root. (maizec ignores the graphical-only\n"
		"                    keys.) console-size is <cols>x<rows> (e.g. 80x25).\n"
		"                    Booleans accept true/false, 1/0, or yes/no. Precedence is\n"
		"                    built-in default < ~/.maize/config < CLI flag (a CLI flag\n"
		"                    always wins). A bad key or value is warned and ignored.\n"
		"  ~/.maize/env      a default guest environment (same KEY=VAL format as\n"
		"                    --env-file) loaded into every guest before -e/--env entries.\n"
		"Both files are optional; absent means the built-in defaults and an empty env.\n"
		"\n"
		"By default the guest gets a persistent sandbox root filesystem: a dedicated\n"
		"host directory (~/.maize/root, created on first run with /home/user and /tmp)\n"
		"is mounted read-write as the guest root '/', and the startup working directory\n"
		"is /home/user. A relative guest path resolves against that cwd, so a program\n"
		"that writes ./file lands under the sandbox root and persists across runs.\n"
		"Your real filesystem is NOT reachable: only the sandbox root plus any explicit\n"
		"--mount / --mount-home overlays (which layer on top of the root). Pass\n"
		"--no-root for the empty, deny-by-default filesystem.\n"
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
	/* maize-170: a KEY defined more than once takes its LAST value. A later -e /
	   --env / --env-file entry (and any CLI env entry over a ~/.maize/env default)
	   overrides an earlier one, replacing it in place so the guest's envp holds a
	   single entry per key with a stable position. Without this the first-wins
	   scan order of a guest getenv would make the standing default shadow a CLI
	   override, defeating the intended default-then-override precedence. */
	std::string prefix = key + "=";
	for (std::string &existing : env) {
		if (existing.size() >= prefix.size()
			&& existing.compare(0, prefix.size(), prefix) == 0) {
			existing = entry;
			return true;
		}
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

/* maize-114: a parsed --mount / --mount-home grant, held in main's scope so the
   hostfs_mount const char* views into these strings stay valid through cpu::run(). */
struct mount_grant {
	std::string host;
	std::string guest;
	hostfs_mode mode;
};

/* maize-114: a guest path must be a *nix absolute path under the synthetic root.
   Rejects a non-'/' start, a drive letter or backslash (a Windows-shaped path), and
   "/" itself (the root is synthetic and cannot be a mount target). Fail-closed. */
static bool valid_guest_path(const std::string &guest, std::string &err) {
	if (guest.empty() || guest[0] != '/') {
		err = "guest path '" + guest + "' is not a *nix absolute path (must begin with '/')";
		return false;
	}
	if (guest == "/") {
		err = "guest path '/' is the synthetic root and cannot be a mount target";
		return false;
	}
	if (guest.find('\\') != std::string::npos || guest.find(':') != std::string::npos) {
		err = "guest path '" + guest + "' contains a drive letter or backslash";
		return false;
	}
	return true;
}

/* maize-114: parse `<host-path>=<guest-path>[:ro|:rw]`. Split on the LAST '=' so a
   Windows host path's drive colon stays on the host side; the optional :ro/:rw suffix
   sets the posture (default :ro). Returns false with a diagnostic in err on a
   malformed spec. */
static bool parse_mount_spec(const std::string &spec, mount_grant &g, std::string &err) {
	std::string::size_type eq = spec.rfind('=');
	if (eq == std::string::npos) {
		err = "malformed --mount '" + spec + "' (expected <host-path>=<guest-path>[:ro|:rw])";
		return false;
	}
	g.host = spec.substr(0, eq);
	std::string rest = spec.substr(eq + 1);
	g.mode = HOSTFS_RO;
	if (rest.size() >= 3 && rest.compare(rest.size() - 3, 3, ":ro") == 0) {
		rest.erase(rest.size() - 3);
	} else if (rest.size() >= 3 && rest.compare(rest.size() - 3, 3, ":rw") == 0) {
		g.mode = HOSTFS_RW;
		rest.erase(rest.size() - 3);
	}
	g.guest = rest;
	if (g.host.empty()) {
		err = "malformed --mount '" + spec + "' (empty host path)";
		return false;
	}
	if (!valid_guest_path(g.guest, err)) {
		return false;
	}
	return true;
}

/* maize-114: true if two guest paths are equal or one is a path-prefix of the other
   (component-aware: "/a" overlaps "/a/b" but not "/ab"). Overlaps are rejected at
   startup because resolution would be ambiguous. */
static bool guest_paths_overlap(const std::string &a, const std::string &b) {
	if (a == b) {
		return true;
	}
	const std::string &shorter = (a.size() < b.size()) ? a : b;
	const std::string &longer  = (a.size() < b.size()) ? b : a;
	if (longer.compare(0, shorter.size(), shorter) == 0
	    && longer[shorter.size()] == '/') {
		return true;
	}
	return false;
}

/* maize-114: validate a fully-collected grant set and build the hostfs mount table.
   Every failure here exits startup nonzero with a diagnostic (fail-closed, doc §1):
   the guest never starts on a bad grant, and a bad grant never degrades to
   mount-nothing or mount-rw. On success fills mounts[] (views into grants[]) and
   returns true. */
static bool build_mount_table(std::vector<mount_grant> &grants,
	std::vector<hostfs_mount> &mounts) {
	for (std::size_t i = 0; i < grants.size(); ++i) {
		std::error_code ec;
		if (!std::filesystem::is_directory(grants[i].host, ec)) {
			std::cerr << "maize: --mount host path '" << grants[i].host
				<< "' is missing or not a directory" << std::endl;
			return false;
		}
	}
	for (std::size_t i = 0; i < grants.size(); ++i) {
		for (std::size_t j = i + 1; j < grants.size(); ++j) {
			if (guest_paths_overlap(grants[i].guest, grants[j].guest)) {
				std::cerr << "maize: --mount guest paths '" << grants[i].guest
					<< "' and '" << grants[j].guest
					<< "' are the same or overlap" << std::endl;
				return false;
			}
		}
	}

	mounts.clear();
	mounts.reserve(grants.size());
	for (std::size_t i = 0; i < grants.size(); ++i) {
		hostfs_mount m;
		m.guest_prefix = grants[i].guest.c_str();
		m.host_root = grants[i].host.c_str();
		m.mode = grants[i].mode;
		m.anchor = nullptr;
		mounts.push_back(m);
	}
	for (std::size_t i = 0; i < mounts.size(); ++i) {
		std::int64_t rc = hostfs_backend_anchor_open(&mounts[i]);
		if (rc < 0) {
			if (rc == -static_cast<std::int64_t>(HOSTFS_ENOSYS)) {
				std::cerr << "maize: hostfs requires openat2 (Linux kernel >= 5.6); "
					<< "cannot mount '" << grants[i].host << "'" << std::endl;
			} else {
				std::cerr << "maize: cannot mount '" << grants[i].host << "' at '"
					<< grants[i].guest << "' (errno " << (-rc) << ")" << std::endl;
			}
			return false;
		}
	}
	return true;
}

/* maize-114: resolve the host home for --mount-home. An explicit override wins;
   otherwise HOME (POSIX) or USERPROFILE (Windows). Returns false if none is set. */
static bool resolve_home(const std::string &override_path, std::string &home) {
	if (!override_path.empty()) {
		home = override_path;
		return true;
	}
	const char *h = std::getenv("HOME");
	if (h == nullptr || h[0] == '\0') {
		h = std::getenv("USERPROFILE");
	}
	if (h == nullptr || h[0] == '\0') {
		return false;
	}
	home = h;
	return true;
}

/* maize-169: parse a config boolean. Accepts true/false, 1/0, yes/no
   (case-insensitive). Returns false if the token is not a recognized boolean so
   the caller can warn and ignore the line (fail-soft). */
static bool parse_config_bool(const std::string &val, bool &out) {
	std::string v;
	for (char c : val) {
		v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	if (v == "true" || v == "1" || v == "yes") { out = true; return true; }
	if (v == "false" || v == "0" || v == "no") { out = false; return true; }
	return false;
}

/* maize-169: seed launcher flag DEFAULTS from ~/.maize/config. The format mirrors
   --env-file: one `key=value` per line, blank lines and lines whose first
   non-whitespace character is '#' ignored. Keys are the long flag names WITHOUT
   the leading dashes. Covered keys (scalar + boolean launcher flags only):
   display-scale, refresh-hz, resolution, console-size, root, show-perf, display,
   input, and the boolean no-root. Precedence is built-in default < config < CLI, so this runs
   BEFORE the CLI argument loop and only changes the value each flag STARTS from; a
   CLI flag always wins because the loop overrides afterward. Fail-soft (per the
   card): an unknown key or a malformed value produces a stderr warning and is
   ignored, never a hard error, so one bad line cannot brick the launcher. The file
   is optional; an absent file leaves the built-in defaults untouched. Repeatable
   --mount grants are intentionally out of scope for v1 (a future extension). */
static void load_config_file(const std::string &path,
	unsigned &display_scale, unsigned &refresh_hz,
	maize::u_hword &fb_width, maize::u_hword &fb_height,
	maize::u_hword &console_width, maize::u_hword &console_height,
	std::string &root_override, bool &show_perf,
	bool &display_requested, std::string &input_source, bool &no_root,
	bool &pause_on_halt) {
	std::ifstream f(path);
	if (!f.is_open()) {
		return;   /* optional file absent: built-in defaults stand (unchanged behavior) */
	}
	std::string line;
	while (std::getline(f, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();   /* tolerate CRLF checkouts, like load_env_file */
		}
		std::string::size_type start = line.find_first_not_of(" \t");
		if (start == std::string::npos) {
			continue;          /* blank line */
		}
		if (line[start] == '#') {
			continue;          /* comment line */
		}
		std::string::size_type eq = line.find('=', start);
		if (eq == std::string::npos) {
			std::cerr << "maize: ignoring malformed config line '" << line.substr(start)
				<< "' in " << path << " (expected key=value)" << std::endl;
			continue;
		}
		/* key = trimmed text before '=' (allow trailing whitespace before the '='). */
		std::string key = line.substr(start, eq - start);
		std::string::size_type kend = key.find_last_not_of(" \t");
		key = (kend == std::string::npos) ? std::string() : key.substr(0, kend + 1);
		/* value = text after '=', surrounding whitespace trimmed. */
		std::string val = line.substr(eq + 1);
		std::string::size_type vstart = val.find_first_not_of(" \t");
		if (vstart == std::string::npos) {
			val.clear();
		} else {
			std::string::size_type vend = val.find_last_not_of(" \t");
			val = val.substr(vstart, vend - vstart + 1);
		}

		if (key == "display-scale") {
			int s = std::atoi(val.c_str());
			if (s < 1 || s > 16) {
				std::cerr << "maize: ignoring config display-scale '" << val
					<< "' (must be between 1 and 16)" << std::endl;
				continue;
			}
			display_scale = static_cast<unsigned>(s);
		} else if (key == "refresh-hz") {
			int hz = std::atoi(val.c_str());
			if (hz < 1 || hz > 1000) {
				std::cerr << "maize: ignoring config refresh-hz '" << val
					<< "' (must be between 1 and 1000)" << std::endl;
				continue;
			}
			refresh_hz = static_cast<unsigned>(hz);
		} else if (key == "resolution") {
			maize::u_hword w = 0;
			maize::u_hword h = 0;
			if (!parse_resolution(val, w, h)) {
				std::cerr << "maize: ignoring config resolution '" << val
					<< "' (expected <width>x<height>, e.g. 320x200)" << std::endl;
				continue;
			}
			fb_width = w;
			fb_height = h;
		} else if (key == "console-size") {
			maize::u_hword cw = 0;
			maize::u_hword ch = 0;
			if (!parse_console_size(val, cw, ch)) {
				std::cerr << "maize: ignoring config console-size '" << val
					<< "' (expected <cols>x<rows>, e.g. 80x25; cols 20..500, rows 10..200)"
					<< std::endl;
				continue;
			}
			console_width = cw;
			console_height = ch;
		} else if (key == "root") {
			if (val.empty()) {
				std::cerr << "maize: ignoring config root (empty host path)" << std::endl;
				continue;
			}
			root_override = val;
		} else if (key == "input") {
			if (val != "sys" && val != "keyboard" && val != "console") {
				std::cerr << "maize: ignoring config input '" << val
					<< "' (expected sys, keyboard, or console)" << std::endl;
				continue;
			}
			input_source = (val == "sys") ? std::string() : val;
		} else if (key == "show-perf") {
			bool b = false;
			if (!parse_config_bool(val, b)) {
				std::cerr << "maize: ignoring config show-perf '" << val
					<< "' (expected true/false, 1/0, or yes/no)" << std::endl;
				continue;
			}
			show_perf = b;
		} else if (key == "pause-on-halt") {
			bool b = false;
			if (!parse_config_bool(val, b)) {
				std::cerr << "maize: ignoring config pause-on-halt '" << val
					<< "' (expected true/false, 1/0, or yes/no)" << std::endl;
				continue;
			}
			pause_on_halt = b;
		} else if (key == "display") {
			bool b = false;
			if (!parse_config_bool(val, b)) {
				std::cerr << "maize: ignoring config display '" << val
					<< "' (expected true/false, 1/0, or yes/no)" << std::endl;
				continue;
			}
			display_requested = b;
		} else if (key == "no-root") {
			bool b = false;
			if (!parse_config_bool(val, b)) {
				std::cerr << "maize: ignoring config no-root '" << val
					<< "' (expected true/false, 1/0, or yes/no)" << std::endl;
				continue;
			}
			no_root = b;
		} else {
			std::cerr << "maize: ignoring unknown config key '" << key
				<< "' in " << path << std::endl;
		}
	}
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
	std::vector<mount_grant> grants;
	bool no_root = false;            // --no-root: disable the default sandbox root
	std::string root_override;       // --root <hostpath>: redirect the sandbox root
#ifdef MAIZE_DISPLAY
	/* maize-217 (Option A): the graphical `maize` binary opens a window by DEFAULT, so the
	   binary name determines the mode (maize = the screen; maizec = the terminal). A config
	   `display=false` still forces headless, and --console-dump overrides to headless below. */
	bool display_requested = true;
#else
	bool display_requested = false;  // headless / maizec build: never a window
#endif
	bool console_dump = false;       // --console-dump: bind the text console headlessly and
	                                 // dump its grid at exit (headless CI self-check channel)
	bool show_perf = false;          // --show-perf: draw guest MIPS + FPS in the window corner
	bool pause_on_halt = false;      // --pause-on-halt: hold the window open after the guest halts
	unsigned display_scale = 3;      // window = framebuffer size * scale (--display-scale)
	unsigned refresh_hz = 60;        // "monitor" refresh + vsync-IRQ rate (--refresh-hz)
	maize::u_hword fb_width = 320;   // framebuffer host config (OQ: default 320x200)
	maize::u_hword fb_height = 200;
	/* maize-140: the text console has its OWN default resolution, distinct from the
	   framebuffer's 320x200 DOOM default (OQ2). 640x400 at font8x16 is an 80x25 cell grid,
	   the >= 80-column floor the console targets; keeping it separate from fb_width/height
	   means the framebuffer default (and DOOM's geometry guard) is untouched. */
	maize::u_hword console_width = 640;
	maize::u_hword console_height = 400;
	std::string input_source;        // "" = SYS console (default); "keyboard" | "console"

	/* maize-169 / maize-170: before parsing CLI options, seed launcher flag
	   DEFAULTS from ~/.maize/config and load ~/.maize/env as the standing default
	   guest environment. Both files are OPTIONAL: absent means today's built-in
	   defaults and an empty starting environment, so behavior is unchanged when the
	   operator has not created them. The home directory is resolved with the same
	   HOME/USERPROFILE resolver used for the sandbox root (resolve_home), so a test
	   can redirect both files by pointing HOME at a scratch dir. Precedence for the
	   flags is built-in default < config < CLI (the loop below overrides whatever the
	   config seeded). For the environment the ~/.maize/env defaults load FIRST and
	   the -e / --env / --env-file entries then append or override them (add_env_entry
	   is last-wins per key). The host's own environment is still NEVER inherited:
	   ~/.maize/env is a standing default the OPERATOR owns, not the ambient host env,
	   so the deny-by-default posture holds. */
	{
		std::string cfg_home;
		if (resolve_home("", cfg_home)) {
			std::filesystem::path base = std::filesystem::path(cfg_home) / ".maize";
			load_config_file((base / "config").string(),
				display_scale, refresh_hz, fb_width, fb_height,
				console_width, console_height,
				root_override, show_perf, display_requested, input_source, no_root,
				pause_on_halt);
			if (show_perf) {
				/* config-supplied default: match the --show-perf side effect (the CLI
				   path also calls this; enable_perf_counter just sets a bool, so a
				   second call from --show-perf is harmless). */
				cpu::enable_perf_counter();
			}
			std::error_code ec;
			std::string env_path = (base / "env").string();
			if (std::filesystem::exists(env_path, ec)) {
				/* maize-170: fail-soft like the config file. A malformed default env
				   file warns and applies no default rather than bricking every guest
				   launch. Load into a temp so a partial parse leaves env_entries
				   empty (the CLI -e/--env entries then start from a clean slate). */
				std::vector<std::string> defaults;
				if (load_env_file(env_path, defaults)) {
					for (const std::string &e : defaults) {
						env_entries.push_back(e);
					}
				} else {
					std::cerr << "maize: ignoring the default env file " << env_path
						<< " (~/.maize/env); no default environment applied" << std::endl;
				}
			}
		}
	}

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
		if (arg == "--mount") {
			if (idx + 1 >= argc) {
				std::cerr << "maize: option '--mount' requires a "
					<< "<host-path>=<guest-path>[:ro|:rw] argument" << std::endl;
				print_usage(std::cerr);
				return 2;
			}
			mount_grant g;
			std::string err;
			if (!parse_mount_spec(argv[idx + 1], g, err)) {
				std::cerr << "maize: " << err << std::endl;
				return 2;
			}
			grants.push_back(g);
			idx += 2;
			continue;
		}
		if (arg.rfind("--mount=", 0) == 0) {
			mount_grant g;
			std::string err;
			if (!parse_mount_spec(arg.substr(8), g, err)) {
				std::cerr << "maize: " << err << std::endl;
				return 2;
			}
			grants.push_back(g);
			++idx;
			continue;
		}
		if (arg == "--mount-home" || arg.rfind("--mount-home=", 0) == 0) {
			std::string override_path;
			if (arg.size() > 12) {           /* "--mount-home=" is 13 chars */
				override_path = arg.substr(13);
			}
			std::string home;
			if (!resolve_home(override_path, home)) {
				std::cerr << "maize: --mount-home: no host home found "
					<< "(set HOME/USERPROFILE or pass --mount-home=<path>)" << std::endl;
				return 2;
			}
			mount_grant g;
			g.host = home;
			g.guest = "/home/user";
			g.mode = HOSTFS_RW;   /* the one rw-by-default convenience (OQ 7790) */
			grants.push_back(g);
			++idx;
			continue;
		}
		if (arg == "--root" || arg.rfind("--root=", 0) == 0) {
			/* maize-132: redirect the sandbox root to a different host dir. */
			std::string val;
			if (arg.rfind("--root=", 0) == 0) {   /* "--root=" is 7 chars */
				val = arg.substr(7);
				++idx;
			}
			else if (idx + 1 < argc) {
				val = argv[idx + 1];
				idx += 2;
			}
			else {
				std::cerr << "maize: option '--root' requires a <host-path> argument"
					<< std::endl;
				print_usage(std::cerr);
				return 2;
			}
			if (val.empty()) {
				std::cerr << "maize: --root requires a non-empty host path" << std::endl;
				return 2;
			}
			root_override = val;
			continue;
		}
		if (arg == "--no-root") {
			/* maize-132: disable the default sandbox root; the guest sees the empty
			   deny-by-default filesystem (only explicit --mount grants). */
			no_root = true;
			++idx;
			continue;
		}
		if (arg == "--display") {
			/* Opt-in host window. Only honored when a display backend is compiled in
			   (MAIZE_DISPLAY); otherwise the run stays headless with a note. */
			display_requested = true;
			++idx;
			continue;
		}
		if (arg == "--console-dump") {
			/* maize-140: headless console-verification channel. Binds the window text
			   console to fd 0/1/2 even without a display backend (the console engine has
			   no SDL dependency), sourcing keystrokes from host stdin as Set-1 scancodes,
			   and dumps the rendered grid as text to stdout at exit. The headless
			   run-ctest self-check uses this to verify VT output + the keymap + cooked/raw
			   delivery; a normal run (no flag) is completely unaffected. */
			console_dump = true;
			++idx;
			continue;
		}
		if (arg == "--show-perf") {
			/* Overlay guest MIPS + FPS in the window corner (windowed mode only) and print
			   peak MIPS/FPS on exit. Enables the per-instruction counter. */
			show_perf = true;
			cpu::enable_perf_counter();
			++idx;
			continue;
		}
		if (arg == "--pause-on-halt" || arg == "--no-pause-on-halt") {
			/* Hold the graphical window open on the guest's final frame (and any --show-perf
			   report) until a keypress or window-close, then exit with the guest code. Default
			   off; graphical binary only (maizec has no window). A CLI flag overrides config. */
			pause_on_halt = (arg == "--pause-on-halt");
			++idx;
			continue;
		}
		if (arg == "--display-scale" || arg.rfind("--display-scale=", 0) == 0) {
			/* Integer window magnification of the framebuffer (the guest still renders at
			   the native resolution; SDL scales the presented frame). The window is also
			   resizable, so this only sets the initial size. */
			std::string val;
			if (arg.size() > 16) {           /* "--display-scale=" is 16 chars */
				val = arg.substr(16);
				++idx;
			}
			else if (idx + 1 < argc) {
				val = argv[idx + 1];
				idx += 2;
			}
			else {
				std::cerr << "maize: --display-scale requires a value" << std::endl;
				return 2;
			}
			int s = std::atoi(val.c_str());
			if (s < 1 || s > 16) {
				std::cerr << "maize: --display-scale must be between 1 and 16" << std::endl;
				return 2;
			}
			display_scale = static_cast<unsigned>(s);
			continue;
		}
		if (arg == "--refresh-hz" || arg.rfind("--refresh-hz=", 0) == 0) {
			/* Window refresh + vsync-IRQ cadence (the "monitor" refresh rate). Lower it for
			   undemanding workloads (a terminal parks longer between wakeups), raise it for
			   smoother pacing. The physical present is still bounded by the host display. */
			std::string val;
			if (arg.rfind("--refresh-hz=", 0) == 0) {   /* "--refresh-hz=" is 13 chars */
				val = arg.substr(13);
				++idx;
			}
			else if (idx + 1 < argc) {
				val = argv[idx + 1];
				idx += 2;
			}
			else {
				std::cerr << "maize: --refresh-hz requires a value" << std::endl;
				return 2;
			}
			int hz = std::atoi(val.c_str());
			if (hz < 1 || hz > 1000) {
				std::cerr << "maize: --refresh-hz must be between 1 and 1000" << std::endl;
				return 2;
			}
			refresh_hz = static_cast<unsigned>(hz);
			continue;
		}
		if (arg == "--resolution" || arg.rfind("--resolution=", 0) == 0) {
			std::string spec;
			if (arg.rfind("--resolution=", 0) == 0) {
				spec = arg.substr(13);
				++idx;
			}
			else {
				if (idx + 1 >= argc) {
					std::cerr << "maize: option '--resolution' requires a <width>x<height> argument"
						<< std::endl;
					print_usage(std::cerr);
					return 2;
				}
				spec = argv[idx + 1];
				idx += 2;
			}
			if (!parse_resolution(spec, fb_width, fb_height)) {
				std::cerr << "maize: invalid --resolution '" << spec
					<< "' (expected <width>x<height>, e.g. 320x200)" << std::endl;
				return 2;
			}
			continue;
		}
		if (arg == "--console-size" || arg.rfind("--console-size=", 0) == 0) {
			/* maize-220: text console geometry in CELLS (<cols>x<rows>, e.g. 132x43),
			   distinct from --resolution which sizes the framebuffer. Default 80x25.
			   The pixel canvas handed to text_console is cols/rows * the font cell size. */
			std::string spec;
			if (arg.rfind("--console-size=", 0) == 0) {   /* "--console-size=" is 15 chars */
				spec = arg.substr(15);
				++idx;
			}
			else {
				if (idx + 1 >= argc) {
					std::cerr << "maize: option '--console-size' requires a <cols>x<rows> argument"
						<< std::endl;
					print_usage(std::cerr);
					return 2;
				}
				spec = argv[idx + 1];
				idx += 2;
			}
			if (!parse_console_size(spec, console_width, console_height)) {
				std::cerr << "maize: invalid --console-size '" << spec
					<< "' (expected <cols>x<rows>, e.g. 80x25; cols 20..500, rows 10..200)"
					<< std::endl;
				return 2;
			}
			continue;
		}
		if (arg == "--input" || arg.rfind("--input=", 0) == 0) {
			std::string src;
			if (arg.rfind("--input=", 0) == 0) {
				src = arg.substr(8);
				++idx;
			}
			else {
				if (idx + 1 >= argc) {
					std::cerr << "maize: option '--input' requires a source argument "
						<< "(sys | keyboard | console)" << std::endl;
					print_usage(std::cerr);
					return 2;
				}
				src = argv[idx + 1];
				idx += 2;
			}
			if (src != "sys" && src != "keyboard" && src != "console") {
				std::cerr << "maize: invalid --input '" << src
					<< "' (expected sys | keyboard | console)" << std::endl;
				return 2;
			}
			input_source = (src == "sys") ? std::string() : src;
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

	/* card maize-132: unless --no-root, mount a dedicated host directory as the guest
	   root "/", read-write, auto-created with a minimal skeleton (/home/user, /tmp) on
	   first run. Default host location ~/.maize/root (HOME/USERPROFILE; a plain host
	   dir the operator can open in a file manager); --root <hostpath> redirects it.
	   Everything the guest writes (DOOM saves, .default.cfg) persists under that dir,
	   and the startup cwd is /home/user, so a relative guest path resolves to a
	   writable location with no per-program config. --mount / --mount-home overlay on
	   top (longest-prefix wins; the root is the fallback). The operator's REAL
	   filesystem is NOT reachable by default: only this sandbox dir plus any explicit
	   overlay grants. root_host + cwd live in main's scope so the const char* views the
	   mount table and hostfs_table hold stay valid through cpu::run(). */
	std::string root_host;
	std::string cwd = "/";
	if (!no_root) {
		std::filesystem::path root;
		bool have_root = true;
		if (!root_override.empty()) {
			root = std::filesystem::path(root_override);
		}
		else {
			std::string home;
			if (resolve_home("", home)) {
				root = std::filesystem::path(home) / ".maize" / "root";
			}
			else {
				/* No host home and no --root: fall back to deny-by-default rather
				   than abort, so a bare invocation still runs. The real FS stays
				   unreachable (secure default); only the sandbox is lost. */
				std::cerr << "maize: no host home for the default sandbox root "
					<< "(set HOME/USERPROFILE, pass --root <dir>, or --no-root); "
					<< "running with an empty filesystem" << std::endl;
				have_root = false;
			}
		}
		if (have_root) {
			std::error_code ec;
			std::filesystem::create_directories(root / "home" / "user", ec);
			std::filesystem::create_directories(root / "tmp", ec);
			if (!std::filesystem::is_directory(root, ec)) {
				std::cerr << "maize: cannot create the sandbox root '"
					<< root.string() << "'" << std::endl;
				return 2;
			}
			root_host = root.string();
			mount_grant rg;
			rg.host = root_host;
			rg.guest = "/";
			rg.mode = HOSTFS_RW;
			grants.push_back(rg);
			cwd = "/home/user";
		}
	}

	/* card maize-114: validate the collected --mount / --mount-home grants and build
	   the hostfs mount table BEFORE any VM setup (fail-closed: a bad grant exits
	   nonzero here, before the guest is loaded or the VM's device/IO machinery is
	   stood up). Even with zero grants an (empty) table is installed so the guest sees
	   the synthetic read-only root with no entries (capability model). mounts + table
	   live in main's scope so their const char* / pointer views stay valid for the
	   whole cpu::run(); the table is installed via sys::set_hostfs_table just before
	   the run below. */
	std::vector<hostfs_mount> hostfs_mounts;
	if (!build_mount_table(grants, hostfs_mounts)) {
		return 2;
	}
	hostfs_table hostfs_tab;
	hostfs_tab.mounts = hostfs_mounts.empty() ? nullptr : hostfs_mounts.data();
	hostfs_tab.count = static_cast<unsigned>(hostfs_mounts.size());
	hostfs_tab.ops = hostfs_backend_ops_get();
	hostfs_tab.cwd = cwd.c_str();   /* maize-132: base for relative-path resolution */

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

	/* A single always-registered loopback test device, solely so OUT/OUTR/IN have a real
	   port to exercise in regression tests. Bare `device` instance with no specialized
	   behavior (src/maize_cpu.h class device). It sits at the documented scratch/test port
	   0x0F (relocated from port 1, which is now the console status port); asm/test_outr_in
	   and asm/test_portio target it directly. */
	cpu::device loopback_test_device;
	cpu::add_device(cpu::loopback_test_port, loopback_test_device);

	/* card maize-21: the system timer, the first interrupt source and the end-to-end
	   proof of the interrupt mechanism. Its three registers attach as separate ports in
	   a reserved low-port block (provisional pinout, finalized with the device pinout
	   work), and it is installed as the active instruction-tick timer the run loop
	   advances once per executed instruction. It powers up disabled (control = 0), so a
	   program that never programs it sees no interrupts and every existing fixture is
	   unaffected. */
	cpu::timer_device system_timer;
	system_timer.irq_vector = cpu::timer_irq_vector;
	cpu::add_device(cpu::timer_port_period, system_timer.period_reg);
	cpu::add_device(cpu::timer_port_control, system_timer.control_reg);
	cpu::add_device(cpu::timer_port_status, system_timer.status_reg);
	cpu::set_active_timer(&system_timer);

	/* Standard host-backed device set (device-plugin API). These are compile-time,
	   statically-linked device models attached at their ratified low-block ports. The
	   console and framebuffer are always present; the framebuffer is memory-backed (pixels
	   live in guest RAM, presented through the control ports). The three block-device ports
	   (0x20-0x22, IRQ 35) are reserved with no backend on this milestone: they are attached
	   as plain passthrough devices so a reachable-but-unbacked access is a defined
	   read-back-last-written outcome rather than a surprise. */
	devices::console_device console;
	console.attach();

	devices::keyboard_device keyboard;
	keyboard.attach();

	devices::framebuffer_device framebuffer(fb_width, fb_height);
	framebuffer.attach();

	/* maize-140: the host/VM text console (approach (i)). It owns its own pixel buffer at
	   the console default resolution (640x400 -> 80x50 cells), separate from the
	   framebuffer. It is bound to fd 0/1/2 (below) only when the window console is active
	   (a compiled-in --display window, or --console-dump for the headless CI self-check);
	   otherwise fd 0/1/2 keep host stdio and this object is inert. Constructed
	   unconditionally so its lifetime spans cpu::run(); binding is what activates it. */
	devices::text_console console_dev(console_width, console_height);

	cpu::device block_lba;
	cpu::device block_data;
	cpu::device block_control;
	cpu::add_device(cpu::block_port_lba, block_lba);
	cpu::add_device(cpu::block_port_data, block_data);
	cpu::add_device(cpu::block_port_control, block_control);

	/* Single active stdin consumer (OQ): exactly one host input device drives stdin per
	   run so the SYS console, the console port device, and the keyboard never race the same
	   fd. Default (empty): no injector runs and the SYS console path reads stdin on demand.
	   A windowed run always sources keyboard events from the window, not stdin. */
#ifdef MAIZE_CONSOLE_ONLY
	/* maize-217/225: maizec is the console-only VM binary. `display` never applies (no window),
	   so drop it. `input=keyboard` binds a host-stdin scancode injector; on an INTERACTIVE
	   terminal that injector blocks forever (the maize-217 hang), so neutralize it there -- but
	   ONLY there. When stdin is a pipe or file (a test feeding scancodes, or a deliberate
	   redirect) the injector is exactly what is wanted, so honor it (this is what lets the
	   harness run the keyboard tests through maizec, maize-225). */
	display_requested = false;
	if (input_source == "keyboard" && stdin_is_interactive()) { input_source.clear(); }
#endif
	if (input_source == "keyboard") {
		/* A windowed keyboard is driven entirely by the SDL thread: push_event latches the
		   scancode and raises the keyboard IRQ, and port_read drains the queue on consume. It
		   therefore needs no per-instruction tick-loop poll, so leave active_input unset for it.
		   Only the headless stdin-injection path (no window) needs the tick-loop poll to read
		   stdin. --display without a compiled backend falls back to headless, so key off the
		   actual windowed path, not display_requested alone. */
#ifdef MAIZE_DISPLAY
		bool windowed_keyboard = display_requested;
#else
		bool windowed_keyboard = false;
#endif
		if (!windowed_keyboard) {
			cpu::set_active_input(&keyboard);
		}
	}
	else if (input_source == "console") {
		cpu::set_active_input(&console);
	}

	/* card maize-114: install the mount table built and validated above (before the
	   guest runs). mazm never calls this, so its hostfs paths stay inert. */
	sys::set_hostfs_table(&hostfs_tab);

	/* maize-140: bind the window text console to fd 0/1/2 when the console is active.
	   Active means a compiled-in --display window (the guest's stdout/stderr become
	   glyphs in the window and physical keys become stdin), OR --console-dump (the
	   headless CI self-check path: the same engine runs with no window, sourcing
	   scancodes from host stdin and dumping the grid at exit). Without either, fd 0/1/2
	   keep host stdio unchanged (pipes, CI, no-display runs). The windowed source is the
	   SDL scancode queue; the headless source is host stdin (each byte one Set-1
	   scancode). A --display run without a compiled backend falls back to headless and
	   does NOT bind the console (host stdio, as before), unless --console-dump is set. */
	/* maize-217 (Option A): --console-dump is the headless grid-capture channel (CI self-check),
	   so it forces the graphical binary's window-by-default off: host stdio + a grid dump, no
	   SDL window. Must run before windowed_console is derived from display_requested. */
	if (console_dump) {
		display_requested = false;
	}
	bool windowed_console = false;
#ifdef MAIZE_DISPLAY
	windowed_console = display_requested;
#endif
	if (windowed_console || console_dump) {
		console_dev.set_input_mode(windowed_console
			? devices::text_console::input_mode::QUEUE
			: devices::text_console::input_mode::STDIN);
		sys::set_console(&console_dev);
	}

	sys::init();

	/* Headless by default (no window, no display dependency). A window is created only
	   when --display is passed AND a display backend is compiled in (MAIZE_DISPLAY);
	   otherwise the run stays headless with a one-line note. In windowed mode the guest
	   runs on a background thread while the SDL2 event loop presents frames and feeds the
	   keyboard. */
	/* maize-217: time the run so --show-perf prints an average-rate report on the headless
	   path too (maizec, or a --display build with no backend). The graphical display::run
	   prints its own richer report (with frames/FPS); used_display suppresses the duplicate. */
	maize::perf::cpu_source cpu_perf; if (show_perf) { maize::perf::reset(); maize::perf::add(&cpu_perf); }
	bool used_display = false;

	if (display_requested) {
#ifdef MAIZE_DISPLAY
		devices::display::run(framebuffer, keyboard, console_dev, display_scale, show_perf, refresh_hz, pause_on_halt);
		used_display = true;
#else
		if (!console_dump) {
			std::cerr << "maize: --display requested but no display backend was compiled in "
				<< "(build with -DMAIZE_DISPLAY=ON); running headless" << std::endl;
		}
		cpu::run();
#endif
	}
	else {
		cpu::run();
	}

	/* maize-217: headless --show-perf report (console binary / no-window runs). Instructions
	   and average MIPS over the wall-clock run; frames/FPS are graphical-only, so they are
	   omitted here. Goes to stderr so it never pollutes the guest's stdout (safe under pipes). */
	if (show_perf && !used_display) {
		maize::perf::emit(std::cerr);
	}
#ifdef __linux__
	/* maize-140: the trailing-newline convenience (a Linux-only readability nicety on
	   host stdout) is suppressed when the console owns fd 1: console output is the grid
	   dump below, and a stray host-stdout newline would corrupt the self-check channel. */
	if (!console_dump) {
		std::cout << std::endl;
	}
#endif

	/* maize-140: headless verification channel. With the console bound but no window,
	   the guest's rendered screen lives only in the host grid; dump it as text so the
	   run-ctest self-check can assert the VT output, keymap, and cooked/raw delivery. */
	if (console_dump) {
		console_dev.dump_text(std::cout);
	}

	sys::exit();

	/* card maize-58: surface main's return value as the host process's own exit
	   status. SYS $3C (sys_exit) recorded the low 8 bits of R0 here; a program
	   that ended via HALT recorded nothing, so this defaults to 0. The host's
	   normal 8-bit process-status truncation applies on top. */
	return sys::exit_code();
}
