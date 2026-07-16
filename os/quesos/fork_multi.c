/* fork_multi.c -- maize-93 AC4 fixture, run UNDER quesOS.
 *
 * Proves waitpid blocks until a specific child exits, that zombies are held until
 * reaped, and that multiple children reap in ANY order. The parent forks three
 * children (exit codes 11, 22, 33) and then reaps them in an order that matches
 * neither their creation nor their exit order (C, A, B): while the parent blocks for C,
 * children A and B exit first and are held as zombies until the parent gets to them.
 * Output on success:
 *   wait-anyorder: PASS
 */

int printf(const char *, ...);
long sys_fork(void);
long sys_wait4(long pid, int *status, long options, long rusage);

int main(void) {
    long a = sys_fork();
    if (a == 0) { return 11; }
    long b = sys_fork();
    if (b == 0) { return 22; }
    long c = sys_fork();
    if (c == 0) { return 33; }

    int st = 0;
    int ok = 1;
    long w;

    w = sys_wait4(c, &st, 0, 0);
    if (w != c || ((st >> 8) & 0xFF) != 33) { ok = 0; }
    w = sys_wait4(a, &st, 0, 0);
    if (w != a || ((st >> 8) & 0xFF) != 11) { ok = 0; }
    w = sys_wait4(b, &st, 0, 0);
    if (w != b || ((st >> 8) & 0xFF) != 22) { ok = 0; }

    printf(ok ? "wait-anyorder: PASS\n" : "wait-anyorder: FAIL\n");
    return 0;
}
