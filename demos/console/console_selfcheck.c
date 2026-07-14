/* demos/console/console_selfcheck.c -- headless CI gate for the first-class graphical
 * console (maize-140).
 *
 * The host text console is host C++ (approach (i)), so unlike terminal_selfcheck.c this
 * fixture cannot read the rendered pixels back from guest RAM. Instead it drives the
 * console through ordinary stdio (write() on fd 1, read() on fd 0, tcgetattr/tcsetattr)
 * exactly as a real program would, and the harness verifies the RESULT two ways:
 *
 *   1. VT output: the guest paints known tokens at known cursor positions (CUP), erases
 *      (EL), scrolls the margin (wrap), etc.; the run maize under `--console-dump` prints
 *      the final grid as text to host stdout, and the harness greps it for the expected
 *      lines (and for the ABSENCE of a pre-clear token, proving ED ESC[2J cleared).
 *   2. Input: the harness injects a Set-1 scancode stream on stdin (each byte one
 *      scancode, the same channel `maize --input=keyboard` uses); the guest reads a cooked
 *      line and then a raw byte, checks them itself, and writes "console: PASS" to the
 *      console (which lands in the grid dump) only when both are correct.
 *
 * Injected scancodes (in run-ctest.sh): 0x23 'h', 0x2C 'Z', 0x0E Backspace, 0x17 'i',
 * 0x1C Enter, 0x2D 'x', then 0x3A CapsLock, 0x1E 'a', 0x02 '1'. In the cooked read the
 * Backspace edits the pending line (erasing the erroneous 'Z'), so it returns "hi\n"; raw
 * mode then returns the single 'x'. The CapsLock make latches Caps Lock (it delivers no
 * byte), so the following 'a' raw-reads as 'A' (letters obey Caps Lock) while the '1'
 * raw-reads as '1' (digits do not), proving the alphabetic-only Caps Lock rule headlessly.
 */
#include "syscall.h"
#include "termios.h"
#include "string.h"

/* write() a NUL-terminated string to fd 1 (the console). */
static void emit(const char *s)
{
	write(1, s, strlen(s));
}

int
main(void)
{
	unsigned char buf[16];
	unsigned char c;
	long n;
	struct termios t;
	int cooked_ok;
	int raw_ok;
	int caps_ok;

	/* ---------------- Phase A: VT output (verified via the grid dump) ---------------- */

	/* A token written BEFORE the clear: ED ESC[2J must wipe it, so it must NOT appear in
	   the final dump. */
	emit("\x1b[1;1HPRECLEAR");
	emit("\x1b[2J");

	/* CUP + basic printable. */
	emit("\x1b[1;1HHELLO");
	/* CR then LF: "AB", carriage return, line feed, "CD" -> "AB" on row 3, "CD" on row 4. */
	emit("\x1b[3;1HAB\r\nCD");
	/* Backspace edits the rendered line: "XYZ", two backspaces, "Q" -> "XQZ" on row 6. */
	emit("\x1b[6;1HXYZ\b\bQ");
	/* Horizontal tab: 'A' at col 0, TAB advances to col 8, 'B' -> "A       B" on row 8. */
	emit("\x1b[8;1HA\tB");
	/* Right-margin wrap: at col 79 print "PQZZZ"; P/Q fill the last two columns and "ZZZ"
	   wraps to the start of row 11. */
	emit("\x1b[10;79HPQZZZ");
	/* SGR basic colors (glyph still renders; the text dump verifies the glyph, color is
	   visual-only): red on blue "SGR" then reset, on row 12. */
	emit("\x1b[12;1H\x1b[31;44mSGR\x1b[0m");
	/* EL erase-to-end: "ERASEME", move to col 4, erase to end of line -> "ERA" on row 14. */
	emit("\x1b[14;1HERASEME\x1b[14;4H\x1b[0K");

	/* ---------------- Phase B: cooked-mode line read (echo + Enter) ---------------- */
	/* Position the echo well below the output markers so it does not overwrite them. */
	emit("\x1b[22;1H");
	n = read(0, buf, sizeof(buf));
	cooked_ok = (n == 3 && buf[0] == 'h' && buf[1] == 'i' && buf[2] == '\n');

	/* ---------------- Phase C: raw-mode byte read (no echo, byte-at-a-time) ---------- */
	raw_ok = 0;
	if (tcgetattr(0, &t) == 0) {
		cfmakeraw(&t);
		if (tcsetattr(0, TCSANOW, &t) == 0) {
			n = read(0, &c, 1);
			raw_ok = (n == 1 && c == 'x');
		}
	}

	/* ---------------- Phase D: Caps Lock (raw mode, alphabetic-only effect) --------- */
	/* Still in raw mode (no echo, VMIN=1). The injected CapsLock make (0x3A) latches Caps
	   Lock without delivering a byte; the following 'a' scancode must therefore read back as
	   'A' (letters follow shift XOR caps), and the '1' scancode as '1' (digits ignore Caps
	   Lock). This is the headless proof of the alphabetic-only rule. */
	caps_ok = 0;
	if (raw_ok) {
		n = read(0, &c, 1);
		if (n == 1 && c == 'A') {
			n = read(0, &c, 1);
			caps_ok = (n == 1 && c == '1');
		}
	}

	/* Result line (on its own row, so the grep is exact). */
	emit("\x1b[24;1H");
	emit((cooked_ok && raw_ok && caps_ok) ? "console: PASS" : "console: FAIL");

	return 0;
}
