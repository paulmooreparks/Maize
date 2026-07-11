/* toolchain/rt/stdlib.h -- freestanding <stdlib.h> slice for the Maize C runtime
 * (maize-76): process termination, the malloc family, and the sbrk heap wrapper.
 *
 * exit/_Exit/abort are the M1 termination funnel (no atexit registry yet; stdio is
 * unbuffered so nothing to flush). abort maps to _exit(134) (128 + SIGABRT); Maize
 * has no signal machinery, recorded as an honest deviation.
 *
 * malloc/free/calloc/realloc are an address-ordered first-fit free list with
 * boundary coalescing over sbrk (decision 7340). sbrk is the increment wrapper
 * over the raw sys_brk stub (decision 7344).
 */
#ifndef MAIZE_STDLIB_H
#define MAIZE_STDLIB_H

#include "stddef.h"

/* Functionally noreturn: each reaches _exit (SYS $3C), which halts the VM. The
 * standard C11 `_Noreturn` keyword is intentionally NOT used: it makes cproc emit
 * a `hlt` block terminator that the pinned qbe -t maize backend predates. _exit
 * itself (syscall.h) is likewise a plain `void` for the same reason. */
void exit(int status);
void _Exit(int status);
void abort(void);

void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

/* The Unix heap primitive: grow/shrink the break by incr bytes, returning the OLD
 * break, or (void *)-1 with errno = ENOMEM on failure. sbrk(0) queries. */
void *sbrk(long incr);

#endif /* MAIZE_STDLIB_H */
