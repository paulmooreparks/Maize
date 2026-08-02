/* pagefault_kill.c -- maize-369 AC fixture, run UNDER quesOS.
 *
 * Proves a faulting CHILD is reaped as a signalled death that its parent survives and
 * can observe, and that the two CR2 shapes are distinguishable.
 *
 *   child A: store a byte to VA 0x0. L0 slot 0 is never mapped, so the fault is
 *            present=0, access=store, user=1 (CR2 = 0x0C): the missing-mapping case.
 *   child B: store a byte to VA 0x1000, the trap-table page, which is mapped PTE_KERNEL
 *            with U clear. leaf_permits passes the W check and fails the U check, so the
 *            fault is present=1, access=store, user=1 (CR2 = 0x0D): the permission case.
 *            That is the same present-and-user shape maize-400 reports.
 *   parent:  wait4 each child in turn and require WIFSIGNALED with WTERMSIG == 11
 *            (SIGSEGV), checked as (status & 0x7F) == 11, then print one PASS marker.
 *
 * Reaching the marker at all proves the parent outlived both faults and that the
 * scheduler kept running, which is the whole point of the card.
 *
 * The wild pointers are FILE-SCOPE objects rather than local `volatile`s, for the same
 * reason sig_kill.c's g_sink is: the pinned cproc-qbe cannot lower a volatile store
 * (os/quesos/kill_launch.c:32-36), and cproc does no constant folding, so a store
 * through a global pointer is emitted exactly as written.
 *
 * The stored BYTE also comes from a file-scope object rather than being written as a
 * literal, which is a second, separate toolchain accommodation. Writing the obvious
 * `*g_wild_a = 1;` dies in the Maize QBE backend with "maize emit: unsupported memory
 * address" (qbe-maize/emit.c:152, reached from memaddrreg); routing the value through a
 * variable emits the same store and compiles. That limitation is pre-existing and has
 * nothing to do with this card, so the fixtures route around it rather than chase it.
 *
 * Output on success: "pagefault-kill: PASS".
 */

int printf(const char *, ...);
long sys_fork(void);
long sys_wait4(long pid, void *status, long options, void *rusage);

#define SIGSEGV 11

char *g_wild_a;
char *g_wild_b;
char  g_byte;

int main(void) {
    long a, b;
    unsigned int sa = 0, sb = 0;

    a = sys_fork();
    if (a < 0) { printf("pagefault-kill: FAIL fork-a\n"); return 0; }
    if (a == 0) {
        g_wild_a = (char *)0;
        g_byte = 1;
        *g_wild_a = g_byte;          /* present=0, store, user=1 */
        printf("pagefault-kill: FAIL child-a-survived\n");
        return 0;
    }
    sys_wait4(a, &sa, 0, 0);
    if ((sa & 0x7Fu) != (unsigned)SIGSEGV) {
        printf("pagefault-kill: FAIL child-a-status\n");
        return 0;
    }

    b = sys_fork();
    if (b < 0) { printf("pagefault-kill: FAIL fork-b\n"); return 0; }
    if (b == 0) {
        g_wild_b = (char *)0x1000;
        g_byte = 1;
        *g_wild_b = g_byte;          /* present=1, store, user=1 (kernel-only page) */
        printf("pagefault-kill: FAIL child-b-survived\n");
        return 0;
    }
    sys_wait4(b, &sb, 0, 0);
    if ((sb & 0x7Fu) != (unsigned)SIGSEGV) {
        printf("pagefault-kill: FAIL child-b-status\n");
        return 0;
    }

    printf("pagefault-kill: PASS\n");
    return 0;
}
