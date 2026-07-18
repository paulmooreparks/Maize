/* loader_guard.c -- maize-251 addendum AC fixture (AC 9509), run UNDER quesOS.
 *
 * Proves the loader's segment-fits-USER_BRK_MAX guard. A child execve's an oversized image
 * (bigimage.mzx, whose BSS ends past USER_BRK_MAX). do_execve pre-validates the MZX header
 * (passes), tears down the caller's old address space, then load_segments hits the guard and
 * returns an error, so do_execve terminates the child via do_exit(127) -- its existing
 * post-teardown convention -- rather than corrupting the kernel image or crashing the VM. The
 * PARENT survives, reaps the child, and confirms the clean exit 127.
 *
 * Output on success: "loader-guard: PASS".
 */

int  printf(const char *, ...);
long sys_fork(void);
long sys_wait4(long pid, int *status, long options, long rusage);
long sys_execve(const char *path, char *const argv[], char *const envp[]);

int main(void) {
    long pid = sys_fork();
    if (pid == 0) {
        char *av[] = { (char *)"/progs/bigimage.mzx", 0 };
        char *ev[] = { 0 };
        sys_execve("/progs/bigimage.mzx", av, ev);
        /* execve returns ONLY on a pre-teardown error (ENOENT/ENOEXEC). The oversized-image
         * guard fires POST-teardown (do_exit(127)), so control must NOT reach here. */
        return 99;
    }
    {
        int status = 0, rc;
        if (sys_wait4(pid, &status, 0, 0) != pid) { printf("loader-guard: FAIL wait\n"); return 0; }
        rc = (status >> 8) & 0xFF;
        if (rc == 127) { printf("loader-guard: PASS\n"); }
        else { printf("loader-guard: FAIL rc=%d\n", rc); }
    }
    return 0;
}
