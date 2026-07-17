/* toolchain/rt/signal.h -- <signal.h> subset for the Maize C runtime.
 *
 * maize-172 shipped a no-op stub (Maize had no signal machinery). maize-174 turns quesOS
 * into a real Phase-2 signal kernel, so this header now offers the POSIX subset a shell or
 * editor reaches for: signal/sigaction (over the guest-only SYS $0D rt_sigaction), the
 * sigset_t helpers, sigprocmask (SYS $0E), raise, and the signal-number constants.
 *
 * These are guest-only: under quesOS a process's SYS traps into quesOS's dispatcher, which
 * implements them. A program run DIRECTLY by the VM (not under quesOS) reaches the native
 * SYS table, where these numbers are unimplemented and return 0/-errno harmlessly (so a
 * native kilo's inert SIGWINCH registration keeps working exactly as before).
 */
#ifndef MAIZE_SIGNAL_H
#define MAIZE_SIGNAL_H

typedef void (*__sighandler_t)(int);
typedef unsigned long sigset_t;

#define SIG_ERR ((__sighandler_t)-1)
#define SIG_DFL ((__sighandler_t)0)
#define SIG_IGN ((__sighandler_t)1)

/* Signal numbers (Linux values). */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGKILL  9
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
/* Defined so an editor that installs a resize handler compiles; quesOS never raises it
 * (the console is a fixed grid). SIGWINCH delivery is maize-232's problem. */
#define SIGWINCH 28

/* sigprocmask `how` values. */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

struct sigaction {
    __sighandler_t sa_handler;
    sigset_t       sa_mask;
    int            sa_flags;
};

/* Guest-only raw stubs (syscall.mazm); forward-declared so this header stays standalone. */
long sys_rt_sigaction(long sig, const void *act, void *oldact);
long sys_rt_sigprocmask(long how, const void *set, void *oldset);
long sys_kill(long pid, long sig);
long sys_getpid(void);

static inline int
sigemptyset(sigset_t *set)
{
    *set = 0;
    return 0;
}

static inline int
sigfillset(sigset_t *set)
{
    *set = ~0ul;
    return 0;
}

static inline int
sigaddset(sigset_t *set, int signum)
{
    *set |= (1ul << (signum - 1));
    return 0;
}

static inline int
sigdelset(sigset_t *set, int signum)
{
    *set &= ~(1ul << (signum - 1));
    return 0;
}

static inline int
sigismember(const sigset_t *set, int signum)
{
    return (int)((*set >> (signum - 1)) & 1ul);
}

static inline int
sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
    return (int)sys_rt_sigaction((long)signum, (const void *)act, (void *)oldact);
}

static inline int
sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    return (int)sys_rt_sigprocmask((long)how, (const void *)set, (void *)oldset);
}

/* signal(): thin sigaction wrapper (glibc convention). Returns the previous handler, or
 * SIG_ERR on failure. */
static inline __sighandler_t
signal(int signum, __sighandler_t handler)
{
    struct sigaction sa, old;
    sa.sa_handler = handler;
    sa.sa_mask = 0;
    sa.sa_flags = 0;
    old.sa_handler = SIG_DFL;
    old.sa_mask = 0;
    old.sa_flags = 0;
    if (sigaction(signum, &sa, &old) != 0) {
        return SIG_ERR;
    }
    return old.sa_handler;
}

static inline int
raise(int sig)
{
    return (int)sys_kill(sys_getpid(), (long)sig);
}

#endif /* MAIZE_SIGNAL_H */
