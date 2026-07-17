/* sig_console.c -- maize-174 AC fixture, run UNDER quesOS with --input=console.
 *
 * Proves Ctrl-C via the CONSOLE DEVICE reaches a compute-bound foreground process that is
 * NOT blocked in read(): the process is the boot foreground group, installs SIGINT and
 * SIGQUIT handlers, prints "armed", then busy-loops WITHOUT reading fd 0. The harness
 * injects a control byte (0x03 or 0x1C) only after seeing "armed"; quesOS's console IRQ
 * recognizes the byte value and raises the signal on the foreground group, the handler
 * runs, and the process reports which signal it caught.
 *
 * Output on success: "sig-console: PASS <n>" (n = 2 for SIGINT, 3 for SIGQUIT).
 *
 * NOT wired into run-ctest.sh (operator decision 9052). The interception path this fixture
 * would exercise is the same signal_fg_group -> deliver -> handler machinery already proven
 * deterministically by sig_handler/sig_default/sig_pgroup via the kill path; the automated
 * --input=console proof against a compute-bound process is blocked by the single-slot
 * interrupt controller (maize-21) and is deferred to maize-240 (Queued interrupt controller).
 */

int printf(const char *, ...);
long sys_write(long fd, const void *buf, long count);
long sys_rt_sigaction(long sig, const void *act, void *oldact);

#define SIGINT  2
#define SIGQUIT 3

long g_caught;
long g_sink;

static void on_int(int s)  { (void)s; g_caught = SIGINT; }
static void on_quit(int s) { (void)s; g_caught = SIGQUIT; }

int main(void) {
    unsigned long act[3];
    long k;

    act[1] = 0; act[2] = 0;
    act[0] = (unsigned long)&on_int;
    if (sys_rt_sigaction(SIGINT, act, 0) != 0) { printf("sig-console: FAIL sigaction-int\n"); return 0; }
    act[0] = (unsigned long)&on_quit;
    if (sys_rt_sigaction(SIGQUIT, act, 0) != 0) { printf("sig-console: FAIL sigaction-quit\n"); return 0; }

    sys_write(1, "armed\n", 6);   /* the harness injects the control byte after this */

    for (k = 0; k < 4000000000; ++k) {   /* compute-bound: never reads fd 0 */
        if (g_caught) {
            printf("sig-console: PASS %d\n", (int)g_caught);
            return 0;
        }
        g_sink = g_sink + k;
    }
    printf("sig-console: FAIL no-signal\n");
    return 0;
}
