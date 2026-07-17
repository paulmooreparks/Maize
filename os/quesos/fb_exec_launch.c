/* fb_exec_launch.c -- maize-236 AC fixture (launcher), run UNDER quesOS.
 *
 * Proves execve() releases a held registration (Decision D5): this process registers a
 * framebuffer (slot 0), then execve()s into fb_exec_target, which immediately registers
 * and must get slot 0 again. If exec did not release the prior claim, the target would
 * either see stale ownership (-EBUSY, same process) or a different slot; getting slot 0
 * proves the claim was released across the exec, not left dangling.
 *
 * The target prints the verdict ("fb-exec: PASS").
 */

int printf(const char *, ...);
long sys_fb_register(void *base);
long sys_execve(const char *path, char **argv, char **envp);

static unsigned int fbbuf[64];

int main(void) {
    long s = sys_fb_register(fbbuf);
    if (s < 0) { printf("fb-exec: FAIL launch-register\n"); return 1; }

    char *argv[] = { "/progs/fb_exec_target.mzx", 0 };
    char *envp[] = { 0 };
    sys_execve(argv[0], argv, envp);

    printf("fb-exec: FAIL (execve returned)\n");
    return 1;
}
