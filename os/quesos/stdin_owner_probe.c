/* stdin_owner_probe.c -- maize-238 Branch A byte-theft proof, run as a BARE-VM guest
 * (directly by maize, NOT under quesOS) on the plain DEFAULT invocation. Under Branch A the
 * console device is the active stdin injector: its on_input_tick eagerly pre-reads host
 * stdin. This program reads fd 0 through SYS $00 (the bare-VM path); sys.cpp routes that
 * through the device's drain_stdin so the eager pre-read latch is the SINGLE host-stdin
 * owner. If bytes were stolen by the eager reader, the echoed stream would be short or
 * corrupt. Piping "hello" must round-trip all five bytes in order.
 * Output on success: stdin-owner: PASS
 */
int  printf(const char *, ...);
long read(int fd, void *buf, unsigned long count);

int main(void) {
    char buf[16];
    long total = 0, n;
    while (total < 5) {
        n = read(0, buf + total, (unsigned long)(5 - total));
        if (n <= 0) { break; }
        total += n;
    }
    if (total == 5 && buf[0] == 'h' && buf[1] == 'e' && buf[2] == 'l'
        && buf[3] == 'l' && buf[4] == 'o') {
        printf("stdin-owner: PASS\n");
    } else {
        printf("stdin-owner: FAIL total=%ld\n", total);
    }
    return 0;
}
