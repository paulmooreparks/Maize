/* stdin_wake_eof.c -- maize-313 AC-7 and AC-29 fixture, run UNDER quesOS.
 *
 * End of input must raise IRQ 33 and wake a parked reader rather than leaving it parked
 * forever. The process reads fd 0 until the read returns 0, which means it is parked on fd 0
 * at the moment the writer closes the pipe: the kernel has reached the idle path and the CPU
 * is in its wait-for-interrupt park, so the only thing that can complete the read is a raise
 * from the host stdin source or from the park hook's own probe.
 *
 * AC-29 runs the same program under MAIZE_FAULT=latch_ready, which forces the rising-edge
 * latch set at the instant the readiness answer goes negative. That is the leaf where the
 * end-of-input branch used to fall through into the rising-edge guard, latch eof_ and raise
 * nothing, after which the early return at the top of on_input_tick meant nothing ever raised
 * again and this reader was stranded.
 *
 * Output on success: stdin-wake-eof: PASS <bytes>
 */

int  printf(const char *, ...);
long sys_read(long fd, void *buf, long count);

int main(void) {
    long total = 0;
    for (;;) {
        char c = 0;
        long r = sys_read(0, &c, 1);
        if (r == 0) { break; }              /* end of input: the wake under test */
        if (r != 1) {
            printf("stdin-wake-eof: FAIL read returned %ld\n", r);
            return 0;
        }
        ++total;
    }
    printf("stdin-wake-eof: PASS %ld\n", total);
    return 0;
}
