/* toolchain/rt/stdlib.h -- freestanding <stdlib.h> slice for the Maize C runtime
 * (maize-76): process termination, the malloc family, and the sbrk heap wrapper.
 *
 * exit/_Exit/abort are the M1 termination funnel. exit() runs the atexit registry
 * (LIFO) before _exit; stdio is unbuffered so there is nothing to flush yet (the
 * buffered-stdio flush-on-exit hook is maize-120, delivered by it registering a
 * flush via atexit). _Exit and abort bypass the registry. abort maps to _exit(134)
 * (128 + SIGABRT); Maize has no signal machinery, recorded as an honest deviation.
 *
 * malloc/free/calloc/realloc are an address-ordered first-fit free list with
 * boundary coalescing over sbrk (decision 7340). sbrk is the increment wrapper
 * over the raw sys_brk stub (decision 7344).
 */
#ifndef MAIZE_STDLIB_H
#define MAIZE_STDLIB_H

#include "stddef.h"

/* Each reaches _exit (SYS $3C), which halts the VM, so control never returns. The
 * C11 `_Noreturn` keyword is honest here: cproc emits a `hlt` block terminator for
 * a _Noreturn definition (and for calls to a _Noreturn-declared function), and the
 * qbe -t maize backend now parses and lowers `hlt` to HALT (maize-102). _exit
 * (syscall.h) carries `_Noreturn` for the same reason. */
_Noreturn void exit(int status);
_Noreturn void _Exit(int status);
_Noreturn void abort(void);

/* Register func to run at normal exit(). Handlers run LIFO before _exit; a full
 * registry (ATEXIT_MAX) makes atexit return nonzero. abort()/_Exit() bypass it. */
int atexit(void (*func)(void));

void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

/* The Unix heap primitive: grow/shrink the break by incr bytes, returning the OLD
 * break, or (void *)-1 with errno = ENOMEM on failure. sbrk(0) queries. */
void *sbrk(long incr);

/* Numeric conversions (maize-142), pure computation over the ctype predicates; no
 * syscalls. atoi is defined via strtol (single conversion core). abs/labs are a
 * modular negate and never touch errno; abs(INT_MIN)/labs(LONG_MIN) are UB per the
 * standard and are not special-cased. strtol is standard C: leading-whitespace skip,
 * optional sign, base 0/2..36 with 0x/0-prefix handling, glibc-style overflow clamp
 * to LONG_MAX/LONG_MIN with errno=ERANGE, endptr set to the first unconsumed char.
 * On no conversion strtol returns 0 and sets *endptr = nptr; an invalid base returns
 * 0 with errno=EINVAL. strtol never clears errno on the success path. */
int  atoi(const char *nptr);
int  abs(int j);
long labs(long j);
long strtol(const char *nptr, char **endptr, int base);

/* maize-144 libc growth for the DOOM boot. getenv always returns NULL: Maize runs
 * with an EMPTY environment (no environ-backed lookup), an honest deviation DOOM
 * tolerates. qsort is a correctness-first, in-place O(n^2) selection sort using a
 * fixed 1-byte swap temp (no VLA, no recursion); a faster sort is a perf-shelf
 * follow-up. atof is strtod-lite: whitespace + sign + integer + optional '.'fraction
 * (no exponent, not full IEEE round-tripping), over the maize-137 signed int<->double
 * float codegen (simple decimals like "3.14" / "-2" / "0.5"). */
char  *getenv(const char *name);
void   qsort(void *base, size_t nmemb, size_t size,
             int (*compar)(const void *, const void *));
double atof(const char *nptr);

/* maize-147 declaration (body in the sibling libc card, maize-148). DOOM's m_misc.c
 * shells a command via system(); Maize has no shell, so maize-148 returns a fixed
 * failure. Declared here so every DOOM TU that calls it finds the visible prototype. */
int system(const char *command);

#endif /* MAIZE_STDLIB_H */
