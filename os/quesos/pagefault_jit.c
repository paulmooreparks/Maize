/* pagefault_jit.c -- maize-369 AC-9 fixture, run UNDER quesOS with the JIT engaged.
 *
 * The COMPILED tier is the tier this card actually has to work on: DOOM runs compiled,
 * and a cause-8 delivery that worked interpreted but not out of a JIT block would be a
 * hole exactly where the card is needed. The other three fixtures all fault on a cold
 * store that runs interpreted, so none of them touches jit_faultsafe_call / jit_dispatch.
 *
 * Shape: the child calls hot_store() well past any sane JIT threshold with the pointer
 * aimed at a real object, so the block holding the store is compiled and hot, then aims
 * the SAME pointer at VA 0 and runs the SAME block once more. The faulting store
 * therefore issues from compiled code. The parent requires WIFSIGNALED with WTERMSIG ==
 * 11 and prints one PASS marker.
 *
 * hot_store has external linkage and stores through a file-scope pointer, so cproc (which
 * does no inlining, no constant folding and no loop-invariant hoisting) emits the store
 * inside the loop body exactly as written. No `volatile`: the pinned cproc-qbe cannot
 * lower a volatile store (os/quesos/kill_launch.c:32-36).
 *
 * Output on success: "pagefault-jit: PASS".
 */

int printf(const char *, ...);
long sys_fork(void);
long sys_wait4(long pid, void *status, long options, void *rusage);

#define SIGSEGV 11
#define NHOT    5000            /* far above the harness's --jit-threshold 50 */

char *g_target_ptr;
char  g_target_obj;
long  g_hot_sink;

/* External linkage so the store cannot be folded into the caller or proven dead. */
void hot_store(long k) {
    *g_target_ptr = (char)(k & 0x7F);
    g_hot_sink = g_hot_sink + k;
}

int main(void) {
    long child;
    unsigned int st = 0;

    child = sys_fork();
    if (child < 0) { printf("pagefault-jit: FAIL fork\n"); return 0; }

    if (child == 0) {
        long k;
        g_target_ptr = &g_target_obj;
        for (k = 0; k < NHOT; ++k) { hot_store(k); }   /* tier the block up */
        g_target_ptr = (char *)0;
        hot_store(1);                                  /* same block, now faulting */
        printf("pagefault-jit: FAIL child-survived\n");
        return 0;
    }

    sys_wait4(child, &st, 0, 0);
    if ((st & 0x7Fu) != (unsigned)SIGSEGV) {
        printf("pagefault-jit: FAIL not-sigsegv\n");
        return 0;
    }
    printf("pagefault-jit: PASS\n");
    return 0;
}
