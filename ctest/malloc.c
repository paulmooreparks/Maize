/* maize-76 AC 7351: malloc family self-check over the sbrk free-list allocator
 * (decision 7340). Covers: allocate-write-read round-trips, free()+re-malloc reuse
 * (address-ordered first-fit returns the just-freed block), free(NULL) no-op,
 * malloc(0) (our contract: a unique freeable pointer), realloc grow/copy,
 * realloc(NULL,n)==malloc(n), realloc(p,0)==free(p) returning NULL, and calloc
 * zeroing a REUSED (previously written then freed) block. Prints a single
 * "malloc PASS" / "malloc FAIL" line. */
#include "stdlib.h"
#include "string.h"
#include "stdio.h"

static int ok = 1;

static void check(int cond) { if (!cond) ok = 0; }

int
main(void)
{
    int i;

    /* allocate-write-read round trip */
    char *a = malloc(100);
    check(a != (void *)0);
    for (i = 0; i < 100; i++)
        a[i] = (char)i;
    for (i = 0; i < 100; i++)
        check(a[i] == (char)i);

    /* free + re-malloc of the same size reuses the freed block (first-fit, and the
       freed block coalesces back into the arena head). */
    free(a);
    char *b = malloc(100);
    check(b == a);

    /* free(NULL) is a no-op (must not crash). */
    free((void *)0);

    /* malloc(0): our contract returns a unique, freeable, non-NULL pointer. */
    void *z = malloc(0);
    check(z != (void *)0);
    free(z);
    free(b);

    /* realloc grow preserves contents and yields a usable larger block. */
    char *r = malloc(16);
    check(r != (void *)0);
    memcpy(r, "0123456789ABCDE", 16);   /* 15 chars + NUL */
    char *r2 = realloc(r, 64);
    check(r2 != (void *)0);
    check(memcmp(r2, "0123456789ABCDE", 16) == 0);
    for (i = 0; i < 64; i++)
        r2[i] = (char)(i + 1);
    for (i = 0; i < 64; i++)
        check(r2[i] == (char)(i + 1));
    free(r2);

    /* realloc(NULL, n) == malloc(n); realloc(p, 0) frees and returns NULL. */
    char *rn = realloc((void *)0, 32);
    check(rn != (void *)0);
    check(realloc(rn, 0) == (void *)0);

    /* calloc zeroes a REUSED block: write 0xFF, free, then calloc the same size and
       confirm every byte is zero (a bump allocator that skips the memset fails here). */
    char *c1 = malloc(48);
    check(c1 != (void *)0);
    memset(c1, (int)0xFF, 48);
    free(c1);
    char *c2 = calloc(48, 1);
    check(c2 != (void *)0);
    {
        int zeroed = 1;
        for (i = 0; i < 48; i++)
            if (c2[i] != 0)
                zeroed = 0;
        check(zeroed);
    }
    free(c2);

    puts(ok ? "malloc PASS" : "malloc FAIL");
    return 0;
}
