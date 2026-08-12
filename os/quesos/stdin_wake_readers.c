/* stdin_wake_readers.c -- maize-313 AC-25 fixture, run UNDER quesOS.
 *
 * The multi-reader trace made executable. Two processes park on fd 0 and two bytes arrive
 * close enough together that ONE level change on the host side covers both. quesos_console_irq
 * completes the FIRST parked BLK_CONSOLE reader and breaks, and console_service_parked
 * completes it through wake_with with no data-port read of its own, so the scan then reaches
 * the idle path with the second byte still pending. On master the instruction-tick pump
 * re-probes and delivers it; a design whose only re-probe rode on a guest code path would
 * park the CPU here with nothing left to raise.
 *
 * What makes this pass is the park hook: cpu::run() probes host stdin once more on its way
 * into the wait-for-interrupt park, so input already pending AT the park raises IRQ 33 before
 * the park rather than after it. Removing that hook is the negative control, on which the
 * second reader never completes and the run hits its timeout.
 *
 * The parent never reads fd 0 itself, so both children are genuinely parked when the bytes
 * land. Output on success: stdin-wake-readers: PASS
 */

int  printf(const char *, ...);
long sys_fork(void);
long sys_read(long fd, void *buf, long count);
long sys_write(long fd, const void *buf, long count);
long sys_wait4(long pid, int *status, long options, long rusage);

int main(void) {
    long a, b;
    int st = 0;
    int ok = 1;
    long w;

    a = sys_fork();
    if (a == 0) {
        char c = 0;
        long r = sys_read(0, &c, 1);
        /* Exit status carries the byte, so the parent can tell which child got which and
         * a lost byte is a distinguishable status rather than a silent pass. */
        if (r != 1) { return 1; }
        return (int)(unsigned char)c;
    }

    b = sys_fork();
    if (b == 0) {
        char c = 0;
        long r = sys_read(0, &c, 1);
        if (r != 1) { return 1; }
        return (int)(unsigned char)c;
    }

    /* Both children are now racing to park on fd 0. The parent parks on wait4, which is a
     * different block kind, so nothing here consumes a console byte. */
    w = sys_wait4(a, &st, 0, 0);
    if (w != a) { ok = 0; }
    else {
        int s = (st >> 8) & 0xFF;
        if (s != 'A' && s != 'B') { ok = 0; }
    }
    {
        int first = (st >> 8) & 0xFF;
        w = sys_wait4(b, &st, 0, 0);
        if (w != b) { ok = 0; }
        else {
            int s = (st >> 8) & 0xFF;
            if (s != 'A' && s != 'B') { ok = 0; }
            /* Both bytes must be delivered, and to different readers. The second reader is
             * the one the trace strands, so a run where both children report the same byte
             * is a failure rather than a near miss. */
            if (s == first) { ok = 0; }
        }
    }

    printf(ok ? "stdin-wake-readers: PASS\n" : "stdin-wake-readers: FAIL\n");
    return 0;
}
