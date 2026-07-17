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

/* maize-94: sig_atomic_t is a standard <signal.h> type (C11 7.14); borrowed oksh
 * uses `volatile sig_atomic_t` for its trap-set flags. int matches the Linux ABI. */
typedef int sig_atomic_t;

#define SIG_ERR ((__sighandler_t)-1)
#define SIG_DFL ((__sighandler_t)0)
#define SIG_IGN ((__sighandler_t)1)

/* Signal numbers (Linux values). maize-94 fills in the rest of the standard Linux
 * table so borrowed userland (oksh's trap.c / siglist.c / signame.c) can name every
 * signal it tabulates. quesOS only actually delivers the job-control / termination
 * subset (maize-174); the others are honest constants a program may reference in a
 * trap table without quesOS ever raising them. */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGIOT   6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGSTKFLT 16
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGXCPU  24
#define SIGXFSZ  25
#define SIGVTALRM 26
#define SIGPROF  27
/* Defined so an editor that installs a resize handler compiles; quesOS never raises it
 * (the console is a fixed grid). SIGWINCH delivery is maize-232's problem. */
#define SIGWINCH 28
#define SIGIO    29
#define SIGPWR   30
#define SIGSYS   31

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

/* sigsuspend (maize-94): borrowed oksh's trap.c pauses here waiting for a signal.
 * Wave-1 quesOS delivers no asynchronous signals to a foreground shell (decision
 * 8947: Ctrl-C is a literal byte, no SIGINT), so an honest sigsuspend would block
 * forever with nothing to wake it. It instead returns -1 immediately (the POSIX
 * "interrupted" contract) so oksh's wait loops make progress; the mask argument is
 * accepted and ignored. Named deviation, not a fake. */
static inline int
sigsuspend(const sigset_t *mask)
{
    (void)mask;
    return -1;
}

#endif /* MAIZE_SIGNAL_H */
