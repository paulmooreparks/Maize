/* toolchain/rt/sys/wait.h -- freestanding <sys/wait.h> for the Maize C runtime (maize-94).
 *
 * The process-reaping half of the maize-93 process model, matching the existing
 * sys/stat.h / sys/types.h / sys/time.h convention (a header declaring prototypes whose
 * bodies live in a .c already in the cc-maize.sh RT set -- here unistd.c). wait/waitpid
 * are thin wrappers over the raw sys_wait4 stub; the status macros decode quesOS's
 * wait_status() encoding (toolchain/rt/SYSCALL-ABI.md, os/quesos/quesos.c): a normal exit
 * is (code & 0xFF) << 8 so WEXITSTATUS is bits 8..15, and a signal death (maize-174) is
 * the low 7 bits = the terminating signal.
 *
 * WIFSIGNALED / WTERMSIG are REAL as of maize-174 (quesOS records term_signal for a
 * default-terminated process); the maize-94 spec's "always false/0" note predates that
 * landing and is discharged here. quesOS never produces the 0x7F "stopped" status today
 * (no job-control stop), so WIFSTOPPED is always false, an honest deviation.
 */
#ifndef MAIZE_SYS_WAIT_H
#define MAIZE_SYS_WAIT_H

#ifndef MAIZE_PID_T_DEFINED
#define MAIZE_PID_T_DEFINED
typedef int pid_t;
#endif

/* wait blocks for any child; waitpid targets a specific child (or -1 = any). Both write
 * the encoded status through `status` when non-NULL and return the reaped pid, or -1 with
 * errno (ECHILD when there is no matching child). Bodies in unistd.c over sys_wait4. */
pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);

/* Status decode (quesOS wait_status(): exit code in bits 8..15; signal in low 7 bits). */
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WTERMSIG(s)     ((s) & 0x7F)
#define WIFEXITED(s)    (WTERMSIG(s) == 0)
#define WIFSIGNALED(s)  (WTERMSIG(s) != 0 && WTERMSIG(s) != 0x7F)
#define WIFSTOPPED(s)   (((s) & 0xFF) == 0x7F)   /* always false today (no job-control stop) */
#define WSTOPSIG(s)     WEXITSTATUS(s)
/* WCOREDUMP (maize-94): quesOS never sets a core-dump flag (no core files), so this is
 * always false. Borrowed oksh's jobs.c reports it in a signal-death message. */
#define WCOREDUMP(s)    0
#define WIFCONTINUED(s) 0

/* waitpid options (Linux values). quesOS's wait4 is a blocking reap; WNOHANG is accepted
 * for source compatibility (a non-blocking poll rides options straight to sys_wait4). */
#define WNOHANG    1
#define WUNTRACED  2

#endif /* MAIZE_SYS_WAIT_H */
