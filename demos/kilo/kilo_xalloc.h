/* kilo_xalloc.h (maize-350): checked allocation wrappers and a shared
 * geometric-growth helper for the kilo editor.
 *
 * On Maize a write through a NULL pointer lands in the process's own low
 * address space rather than faulting, so an unchecked malloc/realloc/strdup
 * failure becomes silent self-corruption instead of an error. Routing every
 * allocation in kilo.c through xmalloc/xrealloc/xstrdup closes that class:
 * each wrapper calls die() (a message plus a nonzero exit) when the
 * underlying allocator returns NULL, so the failure is loud and immediate.
 *
 * Style mirrors the host-side precedent at src/mzcc.c:62-78 (die/xmalloc/
 * xstrdup), adapted for the guest side (exit status 1 to match kilo's own
 * fatal-error convention, and a KILO_XALLOC_TESTING hook so a ctest fixture
 * can prove the die() path without a real memory-exhaustion condition).
 */
#ifndef KILO_XALLOC_H
#define KILO_XALLOC_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#ifdef KILO_XALLOC_TESTING
/* Test-only indirection: a call counter and a configurable failure point let
 * a ctest fixture force a NULL return from the underlying allocator without
 * needing a genuine memory-exhaustion condition (maize-350 AC2). Production
 * kilo.c never defines KILO_XALLOC_TESTING, so this block compiles out of
 * the shipped editor entirely. */
static unsigned long kilo_xalloc_calls = 0;
static unsigned long kilo_xalloc_fail_at = 0; /* 0 = never force failure */

static int kilo_xalloc_should_fail(void) {
    kilo_xalloc_calls++;
    return kilo_xalloc_fail_at && kilo_xalloc_calls == kilo_xalloc_fail_at;
}
#define KILO_RAW_MALLOC(n)     (kilo_xalloc_should_fail() ? NULL : malloc(n))
#define KILO_RAW_REALLOC(p,n)  (kilo_xalloc_should_fail() ? NULL : realloc((p),(n)))
#define KILO_RAW_STRDUP(s)     (kilo_xalloc_should_fail() ? NULL : strdup(s))
#define KILO_DIE_STREAM        stdout   /* run_ctest asserts stdout; see AC2 */
#else
#define KILO_RAW_MALLOC(n)     malloc(n)
#define KILO_RAW_REALLOC(p,n)  realloc((p),(n))
#define KILO_RAW_STRDUP(s)     strdup(s)
#define KILO_DIE_STREAM        stderr   /* matches kilo's existing perror-based
                                           fatal errors, e.g. editorOpen's
                                           "Opening file" perror + exit(1) */
#endif

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(KILO_DIE_STREAM, fmt, ap);
    va_end(ap);
    fputc('\n', KILO_DIE_STREAM);
    exit(1);   /* matches kilo's own existing fatal-error exit code
                  (editorReadKey read failure, editorUpdateRow's
                  line-too-long guard, editorOpen's fopen failure) */
}

static void *xmalloc(size_t n) {
    void *p = KILO_RAW_MALLOC(n ? n : 1);
    if (!p) die("kilo: out of memory (malloc %zu bytes)", n);
    return p;
}

static void *xrealloc(void *ptr, size_t n) {
    void *p = KILO_RAW_REALLOC(ptr, n ? n : 1);
    if (!p) die("kilo: out of memory (realloc %zu bytes)", n);
    return p;
}

static char *xstrdup(const char *s) {
    char *p = KILO_RAW_STRDUP(s);
    if (!p) die("kilo: out of memory (strdup %zu bytes)", strlen(s) + 1);
    return p;
}

/* Compute the next capacity for a geometrically-grown buffer, given the
 * current capacity, the number of units needed to fit, and a floor for the
 * first allocation. Pure function: no I/O, no allocation. Doubles from
 * floor_cap (or from cap once cap > 0) until the result is >= need. Shared
 * by E.row's row-count growth and abuf's byte growth (maize-350). */
static int kilo_next_cap(int cap, int need, int floor_cap) {
    int next = cap > 0 ? cap : floor_cap;
    while (next < need) next *= 2;
    return next;
}

#endif /* KILO_XALLOC_H */
