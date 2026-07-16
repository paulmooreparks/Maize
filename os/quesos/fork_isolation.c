/* fork_isolation.c -- maize-93 AC1 fixture, run UNDER quesOS.
 *
 * Proves fork returns 0 in the child and the child pid in the parent, and that the two
 * processes have SEPARATE address spaces: the child mutates a global, the parent must
 * still observe its own (unchanged) copy after reaping the child. `shared` has external
 * linkage and is read only after the opaque sys_wait4 call, so the compiler genuinely
 * reloads it from memory rather than constant-folding; a broken fork that shared a
 * frame would be caught (the parent would see 999).
 *
 * Compiled by the ordinary cc-maize.sh pipeline (stock .mzx); the process calls trap
 * into quesOS's cause-7 dispatcher. Output on success:
 *   child: wrote 999
 *   fork-isolation: PASS
 */

int printf(const char *, ...);
long sys_fork(void);
long sys_getpid(void);
long sys_wait4(long pid, int *status, long options, long rusage);

long shared = 100;   /* external linkage: forces a real reload after sys_wait4 */

int main(void) {
    long pid = sys_fork();

    if (pid == 0) {
        /* Child: mutate our copy of the global. */
        shared = 999;
        printf("child: wrote 999\n");
        return 5;   /* distinctive exit status the parent checks */
    }

    /* Parent: block until the child exits, then verify isolation + the reaped status. */
    int status = 0;
    long w = sys_wait4(pid, &status, 0, 0);

    if (shared == 100 && pid > 0 && w == pid && ((status >> 8) & 0xFF) == 5) {
        printf("fork-isolation: PASS\n");
    } else {
        printf("fork-isolation: FAIL\n");
    }
    return 0;
}
