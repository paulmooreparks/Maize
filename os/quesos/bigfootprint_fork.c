/* bigfootprint_fork.c -- maize-251 addendum AC fixture (AC 9503), run UNDER quesOS.
 *
 * Proves the generalized do_fork L1-walk copies BOTH per-process regions a large guest uses:
 *   - a big BSS (~320 KiB) in region 0 (the enlarged code/data/BSS/heap span), and
 *   - a deep stack buffer (80 KiB, LARGER than the old fixed 64 KiB stack) in the RELOCATED
 *     stack region (0x05400000+).
 * The buffers are filled with position-dependent patterns and held live across fork(); the
 * child must resume with byte-identical copies of BOTH regions (the pre-addendum region-0-only
 * copy loop would have left the child's relocated stack unmapped/garbage). Parent and child
 * each self-check both regions and print a PASS marker; both exit cleanly.
 *
 * Output on success: "bigfootprint-fork: PASS" (parent) preceded by "bigfootprint-fork: child PASS".
 */

int  printf(const char *, ...);
long sys_fork(void);
long sys_wait4(long pid, int *status, long options, long rusage);

#define BSS_SIZE  0x50000u    /* 320 KiB BSS (region 0): exercises the enlarged region-0 span */
#define STK_SIZE  0x14000u    /* 80 KiB deep-stack buffer: exceeds the OLD 64 KiB stack, so it */
                              /* also proves the relocated 256 KiB stack region copies on fork */

static unsigned char g_bss[BSS_SIZE];

static void fill_region(volatile unsigned char *p, unsigned long n, unsigned seed) {
    unsigned long i;
    for (i = 0; i < n; ++i) { p[i] = (unsigned char)(seed + (unsigned)(i * 31u)); }
}
static int check_region(volatile unsigned char *p, unsigned long n, unsigned seed) {
    unsigned long i;
    for (i = 0; i < n; ++i) {
        if (p[i] != (unsigned char)(seed + (unsigned)(i * 31u))) { return 0; }
    }
    return 1;
}

/* The 80 KiB buffer lives on the stack in this frame (deep, below main's), so it is LIVE
 * across fork(): the child must observe a byte-identical copy of the relocated stack region. */
static int run(void) {
    volatile unsigned char sbuf[STK_SIZE];
    long pid;
    int status = 0, rc, pok;

    fill_region(g_bss, BSS_SIZE, 0xA5);
    fill_region(sbuf, STK_SIZE, 0x5A);

    pid = sys_fork();
    if (pid == 0) {
        int ok = check_region(g_bss, BSS_SIZE, 0xA5) && check_region(sbuf, STK_SIZE, 0x5A);
        printf(ok ? "bigfootprint-fork: child PASS\n" : "bigfootprint-fork: child FAIL\n");
        return ok ? 0 : 1;
    }
    if (sys_wait4(pid, &status, 0, 0) != pid) { printf("bigfootprint-fork: FAIL wait\n"); return 0; }
    rc = (status >> 8) & 0xFF;
    pok = check_region(g_bss, BSS_SIZE, 0xA5) && check_region(sbuf, STK_SIZE, 0x5A);
    if (pok && rc == 0) { printf("bigfootprint-fork: PASS\n"); }
    else { printf("bigfootprint-fork: FAIL pok=%d rc=%d\n", pok, rc); }
    return 0;
}

int main(void) { return run(); }
