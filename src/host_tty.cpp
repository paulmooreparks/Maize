/* maize-228: host-terminal raw/VT mirroring. See host_tty.h. */

#include "host_tty.h"
#include "console_io.h"

#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>
#else
#  include <termios.h>
#  include <unistd.h>
#  include <csignal>
#  include <sys/ioctl.h>
#endif

namespace maize {
	namespace host_tty {

		namespace {
			using namespace maize::console;

			bool g_active = false;      // init() ran against a real interactive terminal
			bool g_inited = false;      // init() has run (once)
			bool g_handlers = false;    // restore handlers registered (once)
			bool g_raw = false;         // guest currently in raw mode (last termios_set)
			int g_esc_count = 0;        // consecutive Ctrl-] seen (host kill escape)
			unsigned char g_image[TERMIOS_SIZE];   // current termios image (get returns this)

			void put_u32(unsigned char* p, unsigned v) {
				p[0] = static_cast<unsigned char>(v & 0xFF);
				p[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
				p[2] = static_cast<unsigned char>((v >> 16) & 0xFF);
				p[3] = static_cast<unsigned char>((v >> 24) & 0xFF);
			}
			unsigned get_u32(const unsigned char* p) {
				return static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8)
					| (static_cast<unsigned>(p[2]) << 16) | (static_cast<unsigned>(p[3]) << 24);
			}

			/* The cooked default the guest sees on its first tcgetattr, so the value it saves
			   and restores on exit puts the terminal back to a normal cooked line discipline. */
			void fill_cooked_default(unsigned char* img) {
				std::memset(img, 0, TERMIOS_SIZE);
				put_u32(img + TERMIOS_OFF_IFLAG, TERMIOS_ICRNL);
				put_u32(img + TERMIOS_OFF_OFLAG, TERMIOS_OPOST | TERMIOS_ONLCR);
				put_u32(img + TERMIOS_OFF_CFLAG, 0);
				put_u32(img + TERMIOS_OFF_LFLAG, TERMIOS_ISIG | TERMIOS_ICANON | TERMIOS_ECHO);
				img[TERMIOS_OFF_CC + TERMIOS_VERASE] = 0x7F;
				img[TERMIOS_OFF_CC + TERMIOS_VEOF] = 0x04;
				img[TERMIOS_OFF_CC + TERMIOS_VMIN] = 1;
				img[TERMIOS_OFF_CC + TERMIOS_VTIME] = 0;
			}

			/* A guest image requests raw mode when it has cleared ICANON. */
			bool image_is_raw(const unsigned char* img) {
				return (get_u32(img + TERMIOS_OFF_LFLAG) & TERMIOS_ICANON) == 0;
			}

#if defined(_WIN32)
			HANDLE g_hin = INVALID_HANDLE_VALUE;
			HANDLE g_hout = INVALID_HANDLE_VALUE;
			DWORD g_orig_in = 0;
			DWORD g_orig_out = 0;

			void apply_host(bool raw) {
				if (!g_active) { return; }
				if (raw) {
					DWORD in = g_orig_in;
					in &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
					in |= ENABLE_VIRTUAL_TERMINAL_INPUT;   // arrow keys etc. as VT escape sequences
					SetConsoleMode(g_hin, in);
					/* A raw guest (kilo) emits its own VT + \r\n, so enable VT output
					   processing (renders ANSI on classic conhost; Windows Terminal is
					   VT-native either way) with newline auto-return off. Only in raw mode:
					   a COOKED guest that writes a bare \n must keep the classic console
					   \n -> CR-LF, or its output stair-steps. */
					SetConsoleMode(g_hout, g_orig_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
				} else {
					SetConsoleMode(g_hin, g_orig_in);
					SetConsoleMode(g_hout, g_orig_out);
				}
			}

			BOOL WINAPI ctrl_handler(DWORD) {
				restore();          // window close / logoff / shutdown: leave the terminal sane
				return FALSE;       // fall through to default handling (process ends)
			}
			LONG WINAPI seh_filter(EXCEPTION_POINTERS*) {
				restore();
				return EXCEPTION_CONTINUE_SEARCH;
			}
#else
			struct termios g_orig_tio;

			void apply_host(bool raw) {
				if (!g_active) { return; }
				struct termios t = g_orig_tio;
				if (raw) {
					/* cfmakeraw, spelled out to avoid a feature-test-macro dependency. */
					t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
					t.c_oflag &= ~OPOST;
					t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
					t.c_cflag &= ~(CSIZE | PARENB);
					t.c_cflag |= CS8;
					/* Blocking single-byte reads: the Maize console model is blocking
					   (maize-167), and read()==0 must mean EOF, never a poll timeout. */
					t.c_cc[VMIN] = 1;
					t.c_cc[VTIME] = 0;
				}
				tcsetattr(STDIN_FILENO, TCSANOW, &t);
			}

			void signal_restore(int sig) {
				restore();
				::signal(sig, SIG_DFL);
				::raise(sig);
			}
#endif

			void register_handlers() {
				if (g_handlers) { return; }
				g_handlers = true;
				std::atexit(restore);
#if defined(_WIN32)
				SetConsoleCtrlHandler(ctrl_handler, TRUE);
				SetUnhandledExceptionFilter(seh_filter);
#else
				::signal(SIGINT, signal_restore);
				::signal(SIGTERM, signal_restore);
				::signal(SIGQUIT, signal_restore);
				::signal(SIGHUP, signal_restore);
				::signal(SIGSEGV, signal_restore);
				::signal(SIGABRT, signal_restore);
#endif
			}
		} // namespace

		void init() {
			if (g_inited) { return; }
			g_inited = true;
			fill_cooked_default(g_image);

#if defined(_WIN32)
			g_hin = GetStdHandle(STD_INPUT_HANDLE);
			g_hout = GetStdHandle(STD_OUTPUT_HANDLE);
			DWORD im = 0, om = 0;
			/* GetConsoleMode succeeds ONLY for a real console handle (not a pipe, file, or the
			   NUL device), so it is a cleaner interactive gate than _isatty (which treats NUL
			   as a tty, the maize-221 quirk). */
			if (g_hin == INVALID_HANDLE_VALUE || g_hout == INVALID_HANDLE_VALUE
				|| !GetConsoleMode(g_hin, &im) || !GetConsoleMode(g_hout, &om)) {
				g_active = false;
				return;
			}
			g_orig_in = im;
			g_orig_out = om;
			/* Do NOT change the output mode here: a cooked guest keeps the classic console
			   \n -> CR-LF. VT output processing is enabled only when a guest goes raw
			   (apply_host), so a plain program that writes bare \n is not stair-stepped. */
			g_active = true;
#else
			if (!isatty(STDIN_FILENO)) {
				g_active = false;
				return;
			}
			if (tcgetattr(STDIN_FILENO, &g_orig_tio) != 0) {
				g_active = false;
				return;
			}
			g_active = true;
#endif
			register_handlers();
		}

		void restore() {
			if (!g_active) { return; }
#if defined(_WIN32)
			SetConsoleMode(g_hin, g_orig_in);
			SetConsoleMode(g_hout, g_orig_out);
#else
			tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_tio);
#endif
		}

		bool active() {
			return g_active;
		}

		void termios_get(unsigned char* image) {
			std::memcpy(image, g_image, TERMIOS_SIZE);
		}

		void termios_set(const unsigned char* image) {
			std::memcpy(g_image, image, TERMIOS_SIZE);
			g_raw = image_is_raw(image);
			g_esc_count = 0;
			apply_host(g_raw);
		}

		bool check_kill_escape(const unsigned char* buf, unsigned long n) {
			if (!g_active || !g_raw) { g_esc_count = 0; return false; }
			for (unsigned long i = 0; i < n; ++i) {
				if (buf[i] == 0x1D) {          // Ctrl-]
					if (++g_esc_count >= 3) { g_esc_count = 0; return true; }
				} else {
					g_esc_count = 0;
				}
			}
			return false;
		}

		bool get_winsize(unsigned short* rows, unsigned short* cols) {
			if (!g_active) { return false; }
#if defined(_WIN32)
			CONSOLE_SCREEN_BUFFER_INFO csbi;
			if (!GetConsoleScreenBufferInfo(g_hout, &csbi)) { return false; }
			int r = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
			int c = csbi.srWindow.Right - csbi.srWindow.Left + 1;
			if (r <= 0 || c <= 0) { return false; }
			*rows = static_cast<unsigned short>(r);
			*cols = static_cast<unsigned short>(c);
			return true;
#else
			struct winsize ws;
			if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0 || ws.ws_col == 0) {
				return false;
			}
			*rows = ws.ws_row;
			*cols = ws.ws_col;
			return true;
#endif
		}

	} // namespace host_tty
} // namespace maize
