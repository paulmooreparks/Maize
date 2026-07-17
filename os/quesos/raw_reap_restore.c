/* raw_reap_restore.c -- maize-250 fixture (AC 9112, way 1), run UNDER quesOS.
 *
 * Proves the killed-TUI console restore: when a raw-mode child dies ABNORMALLY (a signal,
 * not its own clean exit), reap_tail -> restore_console_on_death re-applies the PARENT's
 * termios image to the console, so the console does not stay stranded in the dead child's
 * raw mode. Runs under --console-dump, which binds the grid console's termios (so
 * tcgetattr/tcsetattr return 0 headlessly), mirroring the existing termios_raw fixture.
 *
 *   parent: adopt a known-canonical termios (ISIG|ICANON set) so parent->termios_valid=1
 *           and parent->termios_img is the canonical image; fork.
 *   child:  tcsetattr the console RAW (clear ISIG|ICANON|ECHO), announce readiness on a
 *           pipe, then busy-loop awaiting the kill (its own cleanup NEVER runs).
 *   parent: read readiness; confirm the console really went raw (ICANON clear); SIGTERM the
 *           child; wait4 must report WIFSIGNALED / WTERMSIG==SIGTERM; then tcgetattr the
 *           console again -- ICANON MUST be set once more, proving restore_console_on_death
 *           re-applied the parent's canonical termios over the dead child's raw mode. Since
 *           the child was raw at an abnormal death, the same restore path also emits the
 *           alt-screen-exit sequence to fd 1 (the visible half is proven by the live-pty
 *           scenario; here we assert the deterministic tcsetattr-restore result).
 *
 * Output on success: "raw-reap-restore: PASS".
 */

int  printf(const char *, ...);
long sys_fork(void);
long sys_wait4(long pid, void *status, long options, void *rusage);
long sys_pipe(void *fds);
long sys_read(long fd, void *buf, long count);
long sys_write(long fd, const void *buf, long count);
long sys_close(long fd);
long sys_kill(long pid, long sig);
long sys_tcgetattr(long fd, void *t);
long sys_tcsetattr(long fd, long act, void *t);

#define SIGTERM 15

#define TIO_ISIG   0x0001u
#define TIO_ICANON 0x0002u
#define TIO_ECHO   0x0008u

long g_sink;

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
    unsigned char t[36];
    unsigned lf;
    int a[2];
    long child, k;
    char c = 0;
    unsigned int st = 0;

    /* Parent adopts a known-canonical termios so it owns a valid termios_img to restore. */
    if (sys_tcgetattr(0, t) != 0) { printf("raw-reap-restore: FAIL tcgetattr\n"); return 1; }
    set_lflag(t, lflag_of(t) | TIO_ISIG | TIO_ICANON);
    if (sys_tcsetattr(0, 0, t) != 0) { printf("raw-reap-restore: FAIL parent-tcsetattr\n"); return 1; }

    if (sys_pipe(a) != 0) { printf("raw-reap-restore: FAIL pipe\n"); return 1; }
    child = sys_fork();
    if (child < 0) { printf("raw-reap-restore: FAIL fork\n"); return 1; }

    if (child == 0) {
        unsigned char ct[36];
        sys_close(a[0]);
        if (sys_tcgetattr(0, ct) != 0) { sys_write(a[1], "E", 1); for (;;) { } }
        set_lflag(ct, lflag_of(ct) & ~(TIO_ISIG | TIO_ICANON | TIO_ECHO));
        if (sys_tcsetattr(0, 0, ct) != 0) { sys_write(a[1], "E", 1); for (;;) { } }
        sys_write(a[1], "R", 1);   /* announce: console is raw; ready to be killed */
        for (k = 0; k < 2000000000; ++k) { g_sink = g_sink + k; }   /* await the kill */
        return 0;
    }

    sys_close(a[1]);
    if (sys_read(a[0], &c, 1) != 1 || c != 'R') { printf("raw-reap-restore: FAIL child-setup\n"); return 1; }

    /* The child put the console raw: confirm ICANON is now clear before we kill it. */
    if (sys_tcgetattr(0, t) != 0) { printf("raw-reap-restore: FAIL midget\n"); return 1; }
    if (lflag_of(t) & TIO_ICANON) { printf("raw-reap-restore: FAIL not-raw\n"); return 1; }

    sys_kill(child, SIGTERM);                    /* abnormal death: kilo's cleanup never runs */
    sys_wait4(child, &st, 0, 0);
    if ((st & 0x7Fu) != (unsigned)SIGTERM) { printf("raw-reap-restore: FAIL not-sigterm\n"); return 1; }

    /* The dead child was raw; restore_console_on_death must have re-applied the parent's
     * canonical termios. If it did not, the console is stranded raw and ICANON stays clear. */
    if (sys_tcgetattr(0, t) != 0) { printf("raw-reap-restore: FAIL postget\n"); return 1; }
    if ((lflag_of(t) & TIO_ICANON) == 0) { printf("raw-reap-restore: FAIL not-restored\n"); return 1; }

    printf("raw-reap-restore: PASS\n");
    return 0;
}
