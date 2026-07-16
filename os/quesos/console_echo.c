/* console_echo.c -- maize-93 console-input fixture, run UNDER quesOS.
 *
 * Reads a line from fd 0 one byte at a time and echoes it, then checks the line. Under
 * quesOS fd-0 reads go through the console device's IRQ/status path (vector 33), NOT a
 * native blocking read: when no byte is available the PROCESS parks (and, as the only
 * runnable task, the kernel idle-spins so the console keeps ticking) and the console
 * IRQ delivers each byte and wakes the reader. A native blocking read would instead
 * freeze the whole VM. Run with `--input=console` and piped stdin. Expected output for
 * input "hi\n":
 *   hi
 *   console: PASS
 */

int printf(const char *, ...);
long sys_read(long fd, void *buf, long count);
long sys_write(long fd, const void *buf, long count);

int main(void) {
    char line[64];
    int n = 0;
    for (;;) {
        char c;
        long r = sys_read(0, &c, 1);
        if (r <= 0) { break; }        /* EOF */
        if (n < 63) { line[n++] = c; }
        if (c == '\n') { break; }
    }
    sys_write(1, line, n);            /* echo the line back */

    int ok = (n == 3) && line[0] == 'h' && line[1] == 'i' && line[2] == '\n';
    printf(ok ? "console: PASS\n" : "console: FAIL\n");
    return 0;
}
