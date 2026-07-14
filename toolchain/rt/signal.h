/* toolchain/rt/signal.h -- freestanding <signal.h> stub for the Maize C runtime
 * (maize-172).
 *
 * Maize has no signal machinery (recorded as an honest deviation in stdlib.h, where
 * abort() maps to _exit(134) rather than raising SIGABRT). kilo installs a SIGWINCH
 * handler to repaint on terminal resize; the Maize window console is a fixed-size
 * grid that never resizes, so no SIGWINCH is ever delivered and the handler is
 * correctly never called. signal() is therefore a no-op that records nothing and
 * returns SIG_DFL. It is a static inline so it needs no object in the RT link set;
 * only a TU that includes this header (kilo) emits it. A real signal subsystem is a
 * separate build, out of scope for this editor port.
 */
#ifndef MAIZE_SIGNAL_H
#define MAIZE_SIGNAL_H

typedef void (*__sighandler_t)(int);

#define SIG_ERR ((__sighandler_t)-1)
#define SIG_DFL ((__sighandler_t)0)
#define SIG_IGN ((__sighandler_t)1)

/* Window-change signal number (Linux value). Defined so an editor that installs a
 * resize handler compiles; the console never raises it. */
#define SIGWINCH 28

static inline __sighandler_t
signal(int signum, __sighandler_t handler)
{
	(void)signum;
	(void)handler;
	return SIG_DFL;
}

#endif /* MAIZE_SIGNAL_H */
