/* maize-76 AC 7350: sbrk wrapper self-check over the raw sys_brk (SYS $0C) stub.
 * Covers: sbrk(0) queries the current break, a positive increment grows the heap
 * and returns the PRIOR break (with a store/load round-trip inside the new region),
 * and an increment that overshoots HEAP_CEILING fails -- the VM leaves the break
 * unchanged, so sbrk sees got != req, returns (void*)-1, and sets errno = ENOMEM.
 * Prints a single "sbrk PASS" / "sbrk FAIL" line. */
#include "stdlib.h"
#include "errno.h"
#include "stdio.h"

static int ok = 1;

static void check(int cond) { if (!cond) ok = 0; }

int
main(void)
{
    /* query */
    void *b0 = sbrk(0);
    check(b0 != (void *)-1L);

    /* positive grow returns the OLD break; a fresh query is old + increment. */
    void *prev = sbrk(4096);
    check(prev == b0);
    void *b1 = sbrk(0);
    check((char *)b1 == (char *)b0 + 4096);

    /* the grown region is real memory: store then load at both ends. */
    {
        char *p = (char *)prev;
        p[0] = 'M';
        p[4095] = 'Z';
        check(p[0] == 'M' && p[4095] == 'Z');
    }

    /* ceiling failure: cur + huge overshoots HEAP_CEILING (0xFFFFFFFF00000000).
       cur is a low address (image end), so no 64-bit wrap; the request is strictly
       above the ceiling, the VM returns the unchanged break, and sbrk reports
       ENOMEM. */
    {
        unsigned long huge = 0xFFFFFFFF00000000UL;
        void *fail;
        errno = 0;
        fail = sbrk((long)huge);
        check(fail == (void *)-1L);
        check(errno == ENOMEM);
    }

    /* the failed request left the break unchanged. */
    check(sbrk(0) == b1);

    puts(ok ? "sbrk PASS" : "sbrk FAIL");
    return 0;
}
