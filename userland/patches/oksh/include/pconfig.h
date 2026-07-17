/*
 * pconfig.h (maize-94): Maize-local, HAND-AUTHORED replacement for oksh's
 * autoconf-generated pconfig.h. oksh's ./configure is NOT run under Maize (no
 * native configure environment, no feature-probe compiler harness); instead this
 * file pins every HAVE_* / feature switch to what Maize's freestanding libc
 * (toolchain/rt) actually provides today, so the compiled feature set matches
 * this card's stated wave-1 floor rather than silently drifting from a host probe.
 *
 * Overlay note: this file is copied into the pristine oksh submodule scratch by
 * userland/build-userland.sh (patches/oksh/include -> scratch root). It is never
 * committed into the submodule checkout, keeping the vendored tree re-pinnable.
 *
 * Policy (decision 8942/8946 + the spec's config.h section): no job control, no
 * wide chars, no locale, no curses, no pledge/getauxval, no BSD string/vis/strtonum
 * in libc. Every HAVE_* below that names a libc helper is LEFT OFF so oksh compiles
 * its own vendored compat copy (asprintf.c, confstr.c, reallocarray.c, strlcat.c,
 * strlcpy.c, strtonum.c, vis.c, unvis.c, issetugid.c, siglist.c, signame.c). RT does
 * not define any of those symbols, so there is no duplicate-symbol collision at link.
 */

/*
 * __dead / __attribute__ handling. cc-maize.sh already neutralizes GNU
 * __attribute__ on the cpp line (-D '__attribute__(x)='), so define only __dead
 * here (empty: the qbe-maize backend has no noreturn attribute, and a noreturn
 * function that actually returns would just fall through, which never happens in
 * oksh's exit/err paths).
 */
#ifndef __dead
#define __dead
#endif

/* Maize libc provides none of oksh's BSD-ism helpers; use oksh's own compat. */
/* #define HAVE_ASPRINTF */
/* #define HAVE_CONFSTR */
/* #define HAVE_ISSETUGID */
/* #define HAVE_GETAUXVAL */
/* #define HAVE_PLEDGE */
/* #define HAVE_REALLOCARRAY */
/* #define HAVE_SETRESGID */
/* #define HAVE_SETRESUID */
/* #define HAVE_SIG_T */
/* #define HAVE_SRAND_DETERMINISTIC */
/* #define HAVE_ST_MTIM */
/* #define HAVE_ST_MTIMESPEC */
/* #define HAVE_STRAVIS */
/* #define HAVE_STRLCAT */
/* #define HAVE_STRLCPY */
/* #define HAVE_STRTONUM */
/* #define HAVE_STRUNVIS */
/* #define HAVE_SIGLIST */
/* #define HAVE_SIGNAME */
/* #define HAVE_TIMERADD */
/* #define HAVE_TIMERCLEAR */
/* #define HAVE_TIMERSUB */

/* No curses/termcap on Maize (wave 1 uses the raw-mode emacs line editor directly). */
#define NO_CURSES

/* SMALL (maize-94): oksh's supported minimal build. It drops the features outside the
 * wave-1 floor that also happen to need libc surface Maize does not provide: terminfo
 * screen control (setupterm/tputs/clear_screen, var.c's initcurses), mail checking
 * (mcheck), and the KSH_VERSION/OKSH_VERSION typeset niceties. Crucially it KEEPS the
 * emacs-mode line editor (emacs.c is compiled either way; only its terminfo clear-screen
 * path is SMALL-guarded), so decision 8945's in-scope emacs editing survives. This is
 * the honest wave-1 shell: interactive prompt + pipelines + redirection + builtins with
 * emacs editing, minus the terminfo/mail extras. */
#define SMALL

/* NSIG (maize-94): siglist.c / signame.c include pconfig.h (this file) + <signal.h>
 * but NOT oksh's portable.h, so they cannot see portable.h's NSIG. Define it here so
 * both the sys_siglist/sys_signame table definitions and portable.h's extern decl
 * (which #undef/#redefines NSIG to this same 33) agree on the array bound. */
#ifndef NSIG
#define NSIG 33
#endif
