/* maize-228: host-terminal raw/VT mirroring. See host_tty.h. */

#include "host_tty.h"
#include "console_io.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <csignal>

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
			// maize-174: pending synthetic INTR/QUIT byte set by the POSIX SIGINT/SIGQUIT
			// handler (0 = none). Only a flag write happens in the handler (async-signal-safe).
			//
			// maize-313 (H6): a process-directed signal goes to whichever thread has it
			// unblocked, and with the CPU parked in HALT there is no fd-0 read for it to
			// interrupt, so the thread that RUNS the handler and the thread that TAKES the byte
			// are no longer the same thread. A lock-free atomic is legal in a signal handler and
			// is what makes that hand-off correct; the static_assert is what keeps it legal on a
			// host where int is not lock-free.
			std::atomic<int> g_synth_byte {0};
			static_assert(std::atomic<int>::is_always_lock_free,
				"maize-313 H6: the synthetic-byte slot is written from a signal handler, "
				"so it must be lock-free on every host this builds for");

			// maize-313 (H6): the POSIX stdin source's self-pipe write end, or -1 when no
			// source is running. Written once by stdin_source::start() and read by the signal
			// handler, so it is a volatile sig_atomic_t rather than an atomic: the handler must
			// touch nothing that could be mid-update, and a torn read of this value is not
			// possible for sig_atomic_t by definition. Masking SIGINT everywhere except the
			// watcher would also work, but a blocked signal mask is inherited across fork and
			// preserved across exec, and this process spawns compiler children, so a mask would
			// hand every one of them a blocked SIGINT. The self-pipe costs one write and is
			// correct whichever thread runs the handler.
			volatile sig_atomic_t g_signal_wake_fd = -1;

			// maize-264: best-effort teardown callback (presenter segment + child) invoked from
			// restore() on every exit path. NULL until the console binary registers it.
			void (*g_teardown_hook)() = nullptr;

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

			/* maize-345 (D-6): while the VM owns the console its input queue must carry key
			   records only. A console whose inherited mode has mouse input on and quick-edit
			   off queues a mouse-move record per pointer motion, which can head-block the
			   readiness probe's peek window; in cooked mode that is unrecoverable, because the
			   probe is forbidden from consuming there (the console's own line editor needs
			   those records for history and editing). The guest has no mouse and reads the
			   window size by polling GetConsoleScreenBufferInfo (get_winsize below) rather
			   than by consuming resize records, so masking these two loses nothing. Every
			   other bit passes through unchanged, including the extended-flags and quick-edit
			   bits, and restore() still puts back the unmasked mode init() saved. */
			constexpr DWORD kInputMaskOff = ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT;

			void apply_host(bool raw) {
				if (!g_active) { return; }
				/* The cooked branch needs this as much as the raw one: before maize-345 it
				   wrote g_orig_in back verbatim. */
				DWORD base_in = g_orig_in & ~kInputMaskOff;
				if (raw) {
					DWORD in = base_in;
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
					SetConsoleMode(g_hin, base_in);
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

			// maize-174: convert a cooked-mode Ctrl-C / Ctrl-backslash into a synthetic input
			// byte instead of killing the maize(c) process. Async-signal-safe: a flag write
			// only, no restore()/raise(). The fd-0 read path (interrupted with EINTR because
			// this handler carries no SA_RESTART) delivers the byte via take_synthetic_byte().
			void synth_signal(int sig) {
				if (sig == SIGINT) {
					g_synth_byte.store(0x03, std::memory_order_release);
				}
				else if (sig == SIGQUIT) {
					g_synth_byte.store(0x1C, std::memory_order_release);
				}
				else {
					return;
				}
				// maize-313 (H6): wake the stdin source so the byte reaches a parked guest
				// without a further keystroke. Both the sig_atomic_t read and write() are
				// async-signal-safe, and the write is best effort: a full pipe means a wake is
				// already queued.
				int fd = g_signal_wake_fd;
				if (fd >= 0) {
					const unsigned char b = 1;
					ssize_t ignored = ::write(fd, &b, 1);
					(void)ignored;
				}
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
				// maize-174: SIGINT/SIGQUIT become a synthetic input byte (guest ISIG), NOT a
				// process kill. Installed via sigaction with sa_flags = 0 (NOT signal(), whose
				// glibc SA_RESTART would auto-restart the blocked fd-0 read and defeat the
				// synthetic-byte hand-off). SIGTERM/SIGHUP/SIGSEGV/SIGABRT keep the
				// restore-and-die behavior: those mean the process is actually going down.
				struct sigaction sa;
				sa.sa_handler = synth_signal;
				sigemptyset(&sa.sa_mask);
				sa.sa_flags = 0;
				sigaction(SIGINT, &sa, nullptr);
				sigaction(SIGQUIT, &sa, nullptr);
				::signal(SIGTERM, signal_restore);
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
			/* maize-313 (D-17): close the input-record mask window. maize-345's kInputMaskOff
			   is applied only from termios_set, so between here and the guest's first tcsetattr
			   the INHERITED console mode is in force, and an inherited mode with mouse input on
			   and quick edit off queues a record per pointer motion. On master that is a
			   probe-quality problem. With a watcher on the input handle it becomes an idle-cost
			   problem, because every mouse-move record changes the input buffer and a changed
			   buffer is a raise. Apply the mask now instead. restore() still puts back the
			   unmasked g_orig_in saved above, so the guest-visible and shell-visible behaviour
			   at exit is unchanged, and the guest reads the window size by polling
			   GetConsoleScreenBufferInfo rather than by consuming resize records, so masking
			   loses it nothing. */
			/* A failure here is not fatal, because an unmasked console still delivers keys and
			   the watcher still works; it costs idle wakes for pointer motion, which is what
			   AC-20 measures. Report it rather than swallowing it, so that criterion has a
			   diagnostic to point at instead of an unexplained number. The two GetConsoleMode
			   calls above fail CLOSED because their answers are the restore values. */
			if (!SetConsoleMode(g_hin, g_orig_in & ~kInputMaskOff)) {
				std::cerr << "maize: could not mask console input records (error "
					<< GetLastError() << "); mouse and focus events may cost idle wakes"
					<< std::endl;
			}
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

		void set_teardown_hook(void (*hook)()) {
			g_teardown_hook = hook;
		}

		void restore() {
			/* maize-264: run the presenter teardown on EVERY exit path (normal, atexit, and the
			   signal_restore path). It is a no-op if no segment was ever created, so calling it
			   before the interactive-terminal early-return below is safe and covers a crash on a
			   non-interactive run too. */
			if (g_teardown_hook) { g_teardown_hook(); }
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

		int take_synthetic_byte() {
			int b = g_synth_byte.exchange(0, std::memory_order_acquire);
			if (b == 0) { return -1; }
			return b;
		}

		void set_signal_wake_fd(int fd) {
			g_signal_wake_fd = fd;
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
