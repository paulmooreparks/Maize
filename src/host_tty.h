#pragma once

/* maize-228: mirror the guest's termios onto the REAL host terminal so an interactive
   text-mode guest (kilo, a shell, the borrowed userland) driven by the console binary
   renders in and reads from a real terminal (Windows Terminal, xterm, WSL, macOS
   Terminal, SSH). The SYS $F1/$F2 termios dispatch in sys.cpp routes here when no in-VM
   console device is bound (the console `maize` path) AND stdin is an interactive
   terminal; piped/redirected runs keep byte-clean host stdio and never touch the
   terminal. The original mode is saved at startup and restored on normal exit AND on a
   crash/signal (the sharp edge: a console left in raw mode wrecks the user's shell). */

namespace maize {
	namespace host_tty {

		/* Save the host terminal's original mode and enable VT output processing (so guest
		   ANSI renders on classic Windows conhost; harmless on Windows Terminal and on
		   *nix, which are VT-native). No-op when stdin is not an interactive terminal.
		   Registers the exit + crash/signal restore handlers on first call. Idempotent. */
		void init();

		/* Restore the host terminal to its saved original mode. Idempotent and safe to call
		   from an atexit handler or from a signal / console-control handler. */
		void restore();

		/* True when init() ran against an interactive terminal (the gate the termios
		   syscalls use to decide whether to drive the host terminal vs. return -EBADF). */
		bool active();

		/* SYS $F1 tcgetattr: fill a console::TERMIOS_SIZE-byte image with the current mode
		   (a cooked default before the guest first sets it, else the last image set). */
		void termios_get(unsigned char* image);

		/* SYS $F2 tcsetattr: adopt the guest's termios image, driving the host terminal into
		   raw mode (guest cleared ICANON) or cooked mode (guest set ICANON). */
		void termios_set(const unsigned char* image);

		/* SYS $F6 ttysize: fill *rows / *cols with the host terminal's current size. Returns
		   true when an interactive terminal reports a size, false otherwise (the guest then
		   sees -ENOTTY and can fall back to its VT cursor-probe). */
		bool get_winsize(unsigned short* rows, unsigned short* cols);

		/* maize-228 host-side kill escape. Fed the bytes just read from the real terminal on
		   fd 0. In raw mode (where Ctrl-C is a byte to the guest, so a wedged guest cannot be
		   killed from the same terminal) three consecutive Ctrl-] (0x1D) bytes are the host
		   safety hatch: returns true when the third arrives so the caller can restore the
		   terminal and stop the VM. No-op (returns false) unless active and raw. The bytes are
		   still delivered to the guest; the escape only ARMS the kill. */
		bool check_kill_escape(const unsigned char* buf, unsigned long n);

		/* maize-174: on the real terminal (POSIX), a host SIGINT/SIGQUIT in cooked mode is
		   converted by the installed handler into a pending synthetic INTR (0x03) / QUIT
		   (0x1C) byte instead of killing the VM. take_synthetic_byte() returns that byte
		   (and clears it) or -1 if none, so the fd-0 read path can deliver it as ordinary
		   input on EINTR; the guest's own ISIG layer (quesOS) then raises the signal. Always
		   returns -1 on Windows (which uses the console-control handler instead). */
		int take_synthetic_byte();

	} // namespace host_tty
} // namespace maize
