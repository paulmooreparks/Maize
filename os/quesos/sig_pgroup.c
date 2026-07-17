/* sig_pgroup.c -- maize-174 AC fixture, run UNDER quesOS.
 *
 * Proves process-group scoping of signal delivery, deterministically (kill by negative
 * pid, no console timing): a kill(-pgid, SIGINT) reaches every process in that group and
 * only that group. Also exercises setpgid/getpgid/tcsetpgrp/tcgetpgrp as kernel
 * primitives (Job-control substrate; the console-0x03 -> foreground-group path is proven
 * separately by the sig_console fixtures).
 *
 *   parent forks fg and bg; fg calls setpgid(0,0) to form its own group; bg stays in the
 *   parent's (inherited) group. Both announce readiness. The parent tcsetpgrp(fg_pgid),
 *   confirms tcgetpgrp echoes it, then kill(-fg_pgid, SIGINT): fg terminates (WIFSIGNALED
 *   SIGINT) while bg keeps running (later killed with SIGKILL, WTERMSIG 9).
 *
 * Output on success: "sig-pgroup: PASS".
 */

int printf(const char *, ...);
long sys_fork(void);
long sys_wait4(long pid, void *status, long options, void *rusage);
long sys_pipe(void *fds);
long sys_read(long fd, void *buf, long count);
long sys_write(long fd, const void *buf, long count);
long sys_close(long fd);
long sys_kill(long pid, long sig);
long sys_setpgid(long pid, long pgid);
long sys_getpgid(long pid);
long sys_tcsetpgrp(long pgid);
long sys_tcgetpgrp(void);
long sys_getpid(void);

#define SIGINT  2
#define SIGKILL 9

long g_sink;

static long spawn_ready(int makegroup, int *rpipe) {
    int fds[2];
    long pid;
    if (sys_pipe(fds) != 0) { return -1; }
    pid = sys_fork();
    if (pid < 0) { return -1; }
    if (pid == 0) {
        long k;
        sys_close(fds[0]);
        if (makegroup) { sys_setpgid(0, 0); }   /* own pid becomes the new pgid */
        sys_write(fds[1], "R", 1);
        for (k = 0; k < 2000000000; ++k) { g_sink = g_sink + k; }
        return 0;
    }
    sys_close(fds[1]);
    *rpipe = fds[0];
    return pid;
}

int main(void) {
    int pfg = -1, pbg = -1;
    long fg, bg, fgpgid;
    char c = 0;
    unsigned int sfg = 0, sbg = 0;

    fg = spawn_ready(1, &pfg);   /* foreground: its own process group */
    if (fg < 0) { printf("sig-pgroup: FAIL spawn-fg\n"); return 0; }
    bg = spawn_ready(0, &pbg);   /* background: parent's inherited group */
    if (bg < 0) { printf("sig-pgroup: FAIL spawn-bg\n"); return 0; }

    sys_read(pfg, &c, 1);        /* both armed (readiness) */
    sys_read(pbg, &c, 1);

    fgpgid = sys_getpgid(fg);    /* == fg's pid, since it setpgid(0,0)'d */
    if (fgpgid != fg) { printf("sig-pgroup: FAIL fg-pgid\n"); return 0; }
    if (sys_getpgid(bg) == fgpgid) { printf("sig-pgroup: FAIL bg-in-fg-group\n"); return 0; }

    sys_tcsetpgrp(fgpgid);
    if (sys_tcgetpgrp() != fgpgid) { printf("sig-pgroup: FAIL tcgetpgrp\n"); return 0; }

    sys_kill(-fgpgid, SIGINT);   /* group-scoped: only the fg group */
    if (sys_wait4(fg, &sfg, 0, 0) != fg) { printf("sig-pgroup: FAIL wait-fg\n"); return 0; }
    if ((sfg & 0x7Fu) != (unsigned)SIGINT) { printf("sig-pgroup: FAIL fg-not-sigint\n"); return 0; }

    /* bg must still be alive: kill it for real and see SIGKILL, not the earlier SIGINT. */
    sys_kill(bg, SIGKILL);
    if (sys_wait4(bg, &sbg, 0, 0) != bg) { printf("sig-pgroup: FAIL wait-bg\n"); return 0; }
    if ((sbg & 0x7Fu) != (unsigned)SIGKILL) { printf("sig-pgroup: FAIL bg-wrong-signal\n"); return 0; }

    printf("sig-pgroup: PASS\n");
    return 0;
}
