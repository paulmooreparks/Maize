/* exec_target.c -- maize-93 AC2 fixture (target of execve), run UNDER quesOS.
 *
 * The image exec_launch.c execs into. Verifies argv (3 args) and envp arrived intact,
 * then writes the verdict to fd 5, which exec_launch dup2'd from stdout before the
 * exec. Reaching main at all proves execve replaced the image; the correct argv/envp
 * and the working fd 5 prove the argument marshalling and fd-table survival.
 */

long sys_write(long fd, const void *buf, long count);

static int streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) { return 0; } ++a; ++b; }
    return *a == *b;
}
static long slen(const char *s) { long n = 0; while (s[n]) { ++n; } return n; }

int main(int argc, char **argv, char **envp) {
    int ok = (argc == 3)
             && streq(argv[1], "alpha")
             && streq(argv[2], "beta")
             && envp[0] != 0 && streq(envp[0], "QOSVAR=set");
    const char *msg = ok ? "exec: PASS\n" : "exec: FAIL\n";
    sys_write(5, msg, slen(msg));   /* fd 5 = stdout, inherited across execve */
    return 0;
}
