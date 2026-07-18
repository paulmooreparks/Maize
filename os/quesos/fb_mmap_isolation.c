/* fb_mmap_isolation.c -- maize-238 Family C AC fixture (AC 9186), run UNDER quesOS.
 *
 * Proves the generalized page-table walk's process isolation: two concurrent processes
 * each sys_fb_mmap; both get the SAME VA (FB_MMAP_BASE) in their own address spaces backed
 * by DISTINCT physical frames. The child's window is freshly zeroed (fb memory does not
 * propagate across fork, the region-1 exclusion), each writes a distinct pattern, and the
 * parent's pattern is intact after the child ran -- no cross-process bleed. The new
 * L1-index-1 mapping is genuinely per-process, not accidentally shared.
 * Output on success: fb-isolation: PASS
 */
int  printf(const char *, ...);
long sys_fork(void);
long sys_pipe(int *fds);
long sys_fb_mmap(void);
long sys_fb_geometry(void *out);
long read(int fd, void *buf, unsigned long count);
long write(int fd, const void *buf, unsigned long count);
long sys_wait4(long pid, int *status, long options, long rusage);

int main(void) {
    unsigned int geo[3];
    unsigned long npix, i;
    int sync[2];
    long va, pid;
    unsigned int *fb;
    char c;
    int ok;

    sys_fb_geometry(geo);
    npix = (unsigned long)geo[0] * (unsigned long)geo[1];
    sys_pipe(sync);

    va = sys_fb_mmap();
    if (va < 0) { printf("fb-isolation: FAIL parent-mmap\n"); return 0; }
    fb = (unsigned int *)(unsigned long)va;
    for (i = 0; i < npix; ++i) { fb[i] = 0xAAAAAAAAu; }   /* parent pattern */

    pid = sys_fork();
    if (pid == 0) {
        long vc = sys_fb_mmap();   /* fresh, zeroed frames at the same VA (region 1 not inherited) */
        unsigned int *cfb;
        int prop = 0, selfok = 1;
        if (vc < 0) { write(sync[1], "M", 1); return 0; }
        cfb = (unsigned int *)(unsigned long)vc;
        for (i = 0; i < npix; ++i) { if (cfb[i] != 0) { prop = 1; break; } }   /* no fb propagation */
        for (i = 0; i < npix; ++i) { cfb[i] = 0xBBBBBBBBu; }                    /* child pattern */
        for (i = 0; i < npix; ++i) { if (cfb[i] != 0xBBBBBBBBu) { selfok = 0; break; } }
        write(sync[1], (prop == 0 && selfok) ? "C" : "F", 1);
        return 0;
    }

    read(sync[0], &c, 1);   /* child finished */
    ok = (c == 'C');
    for (i = 0; i < npix; ++i) { if (fb[i] != 0xAAAAAAAAu) { ok = 0; break; } }   /* no bleed */
    printf(ok ? "fb-isolation: PASS\n" : "fb-isolation: FAIL\n");
    { int st; sys_wait4(pid, &st, 0, 0); }
    return 0;
}
