/* bigalloc.c -- maize-251 AC fixture (AC 9314), run UNDER quesOS.
 *
 * Proves sys_bigalloc's ($FF) contract directly:
 *   - a size-0 call returns -EINVAL;
 *   - a single call exceeding BIGALLOC_MAX (16 MiB) returns -ENOMEM without corrupting
 *     kernel state (a later valid allocation still succeeds);
 *   - a successful call returns a VA whose FULL range is writable and reads back correctly;
 *   - the window is excluded from fork's eager copy: the child's first bigalloc returns the
 *     SAME base VA the parent's first did (bigalloc_next was RESET, not inherited) and reads
 *     a fresh zeroed frame, never the parent's written pattern.
 *
 * Compiled by the ordinary cc-maize.sh pipeline (stock .mzx); the calls trap into quesOS's
 * cause-7 dispatcher. Output on success: bigalloc: PASS
 */

int  printf(const char *, ...);
long sys_bigalloc(unsigned long size);
long sys_fork(void);
long sys_wait4(long pid, int *status, long options, long rusage);

#define MB (1024UL * 1024UL)

int main(void) {
    long va, cva, pid;
    unsigned long *p;
    unsigned long i, n;
    int status, child_rc;

    /* size 0 -> -EINVAL (22) */
    if (sys_bigalloc(0) != -22) { printf("bigalloc: FAIL einval\n"); return 0; }

    /* a single request past the 16 MiB per-process window -> -ENOMEM (12) */
    if (sys_bigalloc(17UL * MB) != -12) { printf("bigalloc: FAIL enomem\n"); return 0; }

    /* no corruption: a valid allocation still succeeds after the rejected one, and is the
     * FIRST successful bigalloc so its VA is the window base (captured for the fork check). */
    va = sys_bigalloc(1UL * MB);
    if (va <= 0) { printf("bigalloc: FAIL alloc\n"); return 0; }

    /* the full 1 MiB range (every page) is writable and reads back a position-dependent
     * pattern -- proves each page maps a real, distinct, contiguous frame end to end. */
    p = (unsigned long *)(unsigned long)va;
    n = (1UL * MB) / sizeof(unsigned long);
    for (i = 0; i < n; ++i) { p[i] = 0x5152535455565758UL ^ i; }
    for (i = 0; i < n; ++i) {
        if (p[i] != (0x5152535455565758UL ^ i)) { printf("bigalloc: FAIL readback\n"); return 0; }
    }

    /* fork-exclusion (D-mirror of fb_mmap_va): the child's window is NOT the parent's eager
     * copy. bigalloc_next was reset to the base in do_fork, so the child's first bigalloc
     * returns the SAME base VA the parent's first did, backed by a FRESH zeroed frame. */
    pid = sys_fork();
    if (pid == 0) {
        cva = sys_bigalloc(1UL * MB);
        if (cva != va) { return 1; }                                   /* reset -> same base */
        if (*(unsigned long *)(unsigned long)cva != 0) { return 2; }    /* fresh, not parent's data */
        return 0;
    }

    status = 0;
    if (sys_wait4(pid, &status, 0, 0) != pid) { printf("bigalloc: FAIL wait\n"); return 0; }
    child_rc = (status >> 8) & 0xFF;
    if (child_rc != 0) { printf("bigalloc: FAIL fork-exclusion rc=%d\n", child_rc); return 0; }

    printf("bigalloc: PASS\n");
    return 0;
}
