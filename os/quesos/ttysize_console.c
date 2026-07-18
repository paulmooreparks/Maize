/* ttysize_console.c -- maize-253 fixture, run UNDER quesOS.
 *
 * Proves quesOS forwards the console terminal-size call (SYS $F6 sys_ttysize) to a user
 * process and that, since maize-253, the native provider answers it from the bound console
 * device's own cell grid rather than returning -ENOTTY. Runs under
 * --console-dump --console-size 120x40, which binds a 120-col x 40-row grid console. The
 * fixture calls sys_ttysize on its native fd 0 and asserts success plus ws_row==40 /
 * ws_col==120, so a regression to the old real-terminal-only contract (which would return
 * -ENOTTY under --console-dump) is caught. Mirrors termios_raw.c's forwarding-proof shape.
 * Prints "ttysize-console: PASS". The marker rides the grid dump.
 *
 * struct winsize wire image (8 bytes, little-endian): ws_row @0, ws_col @2, ws_xpixel @4,
 * ws_ypixel @6 (u16 each).
 */

int  printf(const char *, ...);
long sys_ttysize(long fd, void *ws);

static unsigned u16_at(const unsigned char *b, int off) {
    return (unsigned)b[off] | ((unsigned)b[off + 1] << 8);
}

int main(void) {
    unsigned char ws[8];
    unsigned rows, cols;
    long r;

    r = sys_ttysize(0, ws);
    if (r != 0) { printf("ttysize-console: FAIL rv=%ld\n", r); return 1; }

    rows = u16_at(ws, 0);
    cols = u16_at(ws, 2);
    if (rows != 40u || cols != 120u) {
        printf("ttysize-console: FAIL rows=%u cols=%u\n", rows, cols);
        return 1;
    }

    printf("ttysize-console: PASS\n");
    return 0;
}
