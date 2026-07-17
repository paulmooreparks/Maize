/* termios_raw.c -- maize-94 fixture (OQ 8951 operator ruling), run UNDER quesOS.
 *
 * Proves quesOS forwards the console termios calls ($F1 tcgetattr / $F2 tcsetattr) to a
 * user process (before this card a quesOS process got -ENOSYS on them, blocking oksh's
 * raw-mode line editor). Runs under --console-dump, which binds the grid console's
 * termios. The fixture reads the current termios, enters raw mode by clearing
 * ISIG|ICANON|ECHO in c_lflag, sets it back, and re-reads: the round trip must show ISIG
 * cleared, proving tcsetattr's change reached the console through the dispatcher's
 * bounce-buffer path (and, in the kernel, flipped g_tty_isig so 0x03/0x1C are delivered
 * as data rather than intercepted as SIGINT/SIGQUIT). Prints "termios-raw: PASS".
 */

int  printf(const char *, ...);
long sys_tcgetattr(long fd, void *t);
long sys_tcsetattr(long fd, long act, void *t);

#define TIO_ISIG   0x0001u
#define TIO_ICANON 0x0002u
#define TIO_ECHO   0x0008u

/* c_lflag is the 4th little-endian 32-bit word of the wire image (byte offset 12). */
static unsigned lflag_of(const unsigned char *t) {
    return (unsigned)t[12] | ((unsigned)t[13] << 8)
         | ((unsigned)t[14] << 16) | ((unsigned)t[15] << 24);
}
static void set_lflag(unsigned char *t, unsigned lf) {
    t[12] = (unsigned char)(lf & 0xFF);
    t[13] = (unsigned char)((lf >> 8) & 0xFF);
    t[14] = (unsigned char)((lf >> 16) & 0xFF);
    t[15] = (unsigned char)((lf >> 24) & 0xFF);
}

int main(void) {
    unsigned char t[36], t2[36];
    unsigned lf;

    if (sys_tcgetattr(0, t) != 0) { printf("termios-raw: FAIL tcgetattr\n"); return 1; }

    lf = lflag_of(t);
    set_lflag(t, lf & ~(TIO_ISIG | TIO_ICANON | TIO_ECHO));
    if (sys_tcsetattr(0, 0 /* TCSANOW */, t) != 0) { printf("termios-raw: FAIL tcsetattr\n"); return 1; }

    if (sys_tcgetattr(0, t2) != 0) { printf("termios-raw: FAIL tcgetattr2\n"); return 1; }
    if (lflag_of(t2) & TIO_ISIG) { printf("termios-raw: FAIL isig-still-set\n"); return 1; }

    printf("termios-raw: PASS\n");
    return 0;
}
