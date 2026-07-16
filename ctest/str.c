/* maize-76 AC 7352: string.h core self-check. Exercises the mem* family, strlen,
 * the comparators, the copiers (including strncpy padding + truncation), strcat,
 * and strchr/strrchr hit-and-miss. Overlapping memmove is checked in BOTH
 * directions (dst<src forward, dst>src backward). Prints a single "str PASS" line
 * (or "str FAIL" on the first failing check), the capstone self-check pattern. */
#include "string.h"
#include "stdio.h"

static int ok = 1;

static void check(int cond) { if (!cond) ok = 0; }

int
main(void)
{
    char buf[32];
    char b2[32];

    /* memcpy */
    memcpy(buf, "hello", 6);
    check(memcmp(buf, "hello", 6) == 0);

    /* memset */
    memset(buf, 'x', 4);
    buf[4] = '\0';
    check(strcmp(buf, "xxxx") == 0);

    /* memmove forward overlap (dst < src): shift "ABCDEF" left by 2 -> "CDEF" */
    memcpy(buf, "ABCDEF", 7);
    memmove(buf, buf + 2, 5);            /* copies "CDEF\0" over the front */
    check(strcmp(buf, "CDEF") == 0);

    /* memmove backward overlap (dst > src): shift "12345" right by 1 */
    memcpy(buf, "12345", 6);
    memmove(buf + 1, buf, 5);            /* buf becomes "112345"? no: 1 + "12345" */
    check(buf[0] == '1' && buf[1] == '1' && buf[2] == '2' &&
          buf[3] == '3' && buf[4] == '4' && buf[5] == '5');

    /* memcmp ordering */
    check(memcmp("abc", "abd", 3) < 0);
    check(memcmp("abd", "abc", 3) > 0);

    /* --- maize-212: word-at-a-time memcpy/memset/memmove coverage -----------
       The calls above are all under 8 bytes and only ever exercise the byte
       tail. These drive n well past 8, off both aligned and unaligned bases,
       and at the exact tail/body boundary, so the uint64_t word body added in
       string.c is actually run and checked byte-for-byte, not just timed. */

    /* memcpy: n=19 (two words + a 3-byte tail), aligned base; bytes past n
       must stay untouched. */
    {
        unsigned char src[24], dst[24];
        int i;
        for (i = 0; i < 24; i++) { src[i] = (unsigned char)(0x40 + i); dst[i] = 0xEE; }
        memcpy(dst, src, 19);
        for (i = 0; i < 19; i++) check(dst[i] == src[i]);
        for (i = 19; i < 24; i++) check(dst[i] == 0xEE);
    }

    /* memcpy: unaligned src/dst bases (offset 1 and 3), n=19, proving the word
       body is correct off a non-8-aligned start, not just fast there. */
    {
        unsigned char src[32], dst[32];
        int i;
        for (i = 0; i < 32; i++) { src[i] = (unsigned char)(0x60 + i); dst[i] = 0xEE; }
        memcpy(dst + 3, src + 1, 19);
        for (i = 0; i < 19; i++) check(dst[3 + i] == src[1 + i]);
        check(dst[2] == 0xEE && dst[22] == 0xEE);
    }

    /* memcpy: n in {0,1,7,8,9}, exactly at the word/tail boundary. */
    {
        unsigned char src[16], dst[16];
        size_t ns[5];
        int i, k;
        ns[0] = 0; ns[1] = 1; ns[2] = 7; ns[3] = 8; ns[4] = 9;
        for (k = 0; k < 5; k++) {
            size_t n = ns[k];
            for (i = 0; i < 16; i++) { src[i] = (unsigned char)(0x80 + i); dst[i] = 0xEE; }
            memcpy(dst, src, n);
            for (i = 0; i < (int)n; i++) check(dst[i] == src[i]);
            for (i = (int)n; i < 16; i++) check(dst[i] == 0xEE);
        }
    }

    /* memset: n=19 aligned, n=19 off an unaligned base (offset 3), and n in
       {0,1,7,8,9} at the word/tail boundary. */
    {
        unsigned char d[32];
        int i;

        for (i = 0; i < 32; i++) d[i] = 0xEE;
        memset(d, 0xAB, 19);
        for (i = 0; i < 19; i++) check(d[i] == 0xAB);
        for (i = 19; i < 32; i++) check(d[i] == 0xEE);

        for (i = 0; i < 32; i++) d[i] = 0xEE;
        memset(d + 3, 0xCD, 19);
        for (i = 0; i < 3; i++) check(d[i] == 0xEE);
        for (i = 3; i < 22; i++) check(d[i] == 0xCD);
        for (i = 22; i < 32; i++) check(d[i] == 0xEE);

        {
            size_t ns[5];
            int k;
            ns[0] = 0; ns[1] = 1; ns[2] = 7; ns[3] = 8; ns[4] = 9;
            for (k = 0; k < 5; k++) {
                size_t n = ns[k];
                for (i = 0; i < 16; i++) d[i] = 0xEE;
                memset(d, 0x55, n);
                for (i = 0; i < (int)n; i++) check(d[i] == 0x55);
                for (i = (int)n; i < 16; i++) check(d[i] == 0xEE);
            }
        }
    }

    /* memmove forward (d<s), n=19 (>=16, crosses the word/tail boundary),
       overlapping and off an unaligned base. */
    {
        unsigned char buf[48], expect[48];
        int i;
        for (i = 0; i < 48; i++) buf[i] = (unsigned char)(0x10 + i);
        for (i = 0; i < 48; i++) expect[i] = buf[i];
        memmove(buf + 1, buf + 5, 19);            /* d < s, overlapping */
        for (i = 0; i < 19; i++) check(buf[1 + i] == expect[5 + i]);
        check(buf[0] == expect[0]);               /* below dst: untouched */
        check(buf[20] == expect[20]);              /* past the moved span */
    }

    /* memmove backward (d>s), n=19 (>=16), confirming the unchanged byte
       loop still holds now that it is the only unoptimized branch left. */
    {
        unsigned char buf[48], expect[48];
        int i;
        for (i = 0; i < 48; i++) buf[i] = (unsigned char)(0x30 + i);
        for (i = 0; i < 48; i++) expect[i] = buf[i];
        memmove(buf + 5, buf + 1, 19);            /* d > s, overlapping */
        for (i = 0; i < 19; i++) check(buf[5 + i] == expect[1 + i]);
        check(buf[0] == expect[0]);               /* below src: untouched */
        check(buf[24] == expect[24]);              /* past the moved span */
    }

    /* strlen */
    check(strlen("") == 0);
    check(strlen("maize") == 5);

    /* strcmp / strncmp */
    check(strcmp("abc", "abc") == 0);
    check(strcmp("abc", "abd") < 0);
    check(strncmp("abcXX", "abcYY", 3) == 0);
    check(strncmp("abcXX", "abcYY", 4) != 0);

    /* strcpy */
    strcpy(b2, "copyme");
    check(strcmp(b2, "copyme") == 0);

    /* strncpy truncation (no NUL when src longer than n) + padding (NUL fill) */
    memset(b2, '?', sizeof b2);
    strncpy(b2, "abcdef", 3);            /* truncates: b2[0..2] = "abc", no NUL added */
    check(b2[0] == 'a' && b2[1] == 'b' && b2[2] == 'c' && b2[3] == '?');
    memset(b2, '?', sizeof b2);
    strncpy(b2, "hi", 5);                /* pads: "hi" then 3 NULs */
    check(b2[0] == 'h' && b2[1] == 'i' && b2[2] == '\0' &&
          b2[3] == '\0' && b2[4] == '\0');

    /* strcat / strncat */
    strcpy(b2, "foo");
    strcat(b2, "bar");
    check(strcmp(b2, "foobar") == 0);
    strcpy(b2, "foo");
    strncat(b2, "barbaz", 3);
    check(strcmp(b2, "foobar") == 0);

    /* strchr / strrchr hit and miss. `s` is a LOCAL array (a stack address), not a
       string literal: comparing strchr's result against s + N must stay a runtime
       stack-relative address. A string-literal source would let qbe copy-propagate
       the literal's symbol address and fold s + N into a `$sym + N` operand, which
       the pinned qbe -t maize backend cannot emit (emit.c: nonzero address offset).
       That backend gap is a fixture-authoring constraint here, not a library limit. */
    {
        char sbuf[] = "banana";
        const char *s = sbuf;
        check(strchr(s, 'b') == s);
        check(strchr(s, 'n') == s + 2);
        check(strrchr(s, 'n') == s + 4);
        check(strchr(s, 'z') == (void *)0);
        check(strrchr(s, 'z') == (void *)0);
        check(strchr(s, '\0') == s + 6);   /* strchr finds the terminator */
    }

    /* memchr hit and miss */
    check(memchr("abcdef", 'd', 6) != (void *)0);
    check(memchr("abcdef", 'z', 6) == (void *)0);

    /* --- maize-100: strstr / strpbrk / strspn / strcspn / strtok(_r) --------- */

    /* strstr: hit, miss, empty-needle-returns-haystack, and a backtracking match.
       Pointer-returning cases use LOCAL mutable buffers (stack addresses), so the
       results stay runtime addresses; a string-literal haystack could let qbe fold
       the result into a `$sym + N` operand the pinned qbe -t maize backend cannot
       emit (see the strchr authoring note above). */
    {
        char hay[] = "aaab";                 /* forces strstr to backtrack */
        char h2[]  = "hello world";
        check(strstr(h2, "world") == h2 + 6);      /* hit */
        check(strstr(h2, "xyz") == (void *)0);     /* miss */
        check(strstr(h2, "") == h2);               /* empty needle -> haystack */
        check(strstr(hay, "aab") == hay + 1);      /* skip "aa", match "aab" at +1 */
    }

    /* strpbrk hit / miss (result is a pointer into a local buffer). */
    {
        char s[] = "hello, world";
        check(strpbrk(s, ",;") == s + 5);          /* first ',' */
        check(strpbrk(s, "xyz") == (void *)0);     /* none of x/y/z present */
    }

    /* strspn / strcspn boundary runs (return lengths, not literal pointers). */
    {
        char s[] = "  abc123";
        check(strspn(s, " ") == 2);                /* two leading spaces */
        check(strspn("abcxyz", "abc") == 3);       /* run of a/b/c */
        check(strcspn(s, "c") == 4);               /* "  ab" then the first 'c' */
        check(strcspn("abcdef", "xyz") == 6);      /* no reject byte -> full length */
    }

    /* strtok_r: reentrant split of "a,,b,c" on "," (consecutive delimiters
       collapse) INTERLEAVED with a second, independent tokenization to prove the
       caller-owned saveptr keeps the two scans separate. Inputs are local mutable
       buffers (strtok_r writes NULs in place; string literals are read-only). */
    {
        char t1[] = "a,,b,c";
        char t2[] = "x-y-z";
        char *s1 = (void *)0, *s2 = (void *)0;
        char *r;

        r = strtok_r(t1, ",", &s1);  check(r && strcmp(r, "a") == 0);
        r = strtok_r(t2, "-", &s2);  check(r && strcmp(r, "x") == 0);
        r = strtok_r((void *)0, ",", &s1);  check(r && strcmp(r, "b") == 0);  /* skips ",," */
        r = strtok_r((void *)0, "-", &s2);  check(r && strcmp(r, "y") == 0);
        r = strtok_r((void *)0, ",", &s1);  check(r && strcmp(r, "c") == 0);
        r = strtok_r((void *)0, "-", &s2);  check(r && strcmp(r, "z") == 0);
        check(strtok_r((void *)0, ",", &s1) == (void *)0);   /* t1 exhausted */
        check(strtok_r((void *)0, "-", &s2) == (void *)0);   /* t2 exhausted */
    }

    /* strtok wrapper over the same input, proving the file-scope static saveptr. */
    {
        char t[] = "a,,b,c";
        char *r;
        r = strtok(t, ",");         check(r && strcmp(r, "a") == 0);
        r = strtok((void *)0, ",");  check(r && strcmp(r, "b") == 0);
        r = strtok((void *)0, ",");  check(r && strcmp(r, "c") == 0);
        check(strtok((void *)0, ",") == (void *)0);
    }

    puts(ok ? "str PASS" : "str FAIL");
    return 0;
}
