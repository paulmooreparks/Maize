/* sig_exec_target.c -- maize-174 AC fixture (execve target), run UNDER quesOS.
 *
 * The image sig_exec_launch's child execs into. It installs NO signal handler; the
 * launcher's caught SIGINT handler must have been reset to SIG_DFL by execve, so the
 * SIGINT the parent sends here default-terminates this process. Announces readiness on
 * fd 5 (a pipe write end the launcher dup2'd before execve, inherited across it) so the
 * parent only signals once the post-exec image is running.
 */

long sys_write(long fd, const void *buf, long count);

long g_sink;

int main(void) {
    long k;
    sys_write(5, "T", 1);   /* readiness on the inherited pipe write end (fd 5) */
    for (k = 0; k < 2000000000; ++k) { g_sink = g_sink + k; }   /* no handler: SIGINT terminates */
    return 0;
}
