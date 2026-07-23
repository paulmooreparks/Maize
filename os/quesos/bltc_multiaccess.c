/* bltc_multiaccess.c -- maize-353 AC 9838 / AC 9845 fixture, run UNDER quesOS (paged / Sv48).
 *
 * Approach A (the block-local translate cache, BLTC) wins where a single straight-line block
 * does SEVERAL same-page memory accesses off one base: struct-field and buffer-processing
 * code, the mundane-app workload class the operator values alongside graphics (decision 9846).
 * This fixture is that shape, and is where A's win shows (open_question 9843 documents the
 * DOOM tight-loop case staying flat, which is expected, not a regression).
 *
 * Each inner step reads eight adjacent longs of a record (eight same-page loads inside one
 * straight-line block) and writes two of them back (two same-page stores). Under paging the
 * JIT emits ONE full fast-page walk per access kind for the block and a cheap BLTC recompare
 * for every later same-kind access in that block, so the redundant per-access translate work
 * drops sharply. Two proofs:
 *   (1) a deterministic PASS plus an accumulator hash, checked byte-identical between --jit
 *       and the interpreter (correctness under the cache), and
 *   (2) the JIT report's "paged translates" line shows a nonzero BLTC-recompare count, i.e.
 *       the mechanism actually engages (the harness greps for it).
 */

int printf(const char *, ...);

#define NREC  2048
#define NPASS 400

struct rec { long a, b, c, d, e, f, g, h; };
struct rec g_recs[NREC];

/* External linkage so the accumulate never folds away. */
long g_sink;

int main(void) {
    long i, pass;

    for (i = 0; i < NREC; ++i) {
        struct rec *r = &g_recs[i];
        r->a = i;         r->b = i * 3 + 1; r->c = i ^ 0x55; r->d = i + 7;
        r->e = i * 5 - 2; r->f = i - 3;     r->g = i & 0xAA; r->h = i | 1;
    }

    long acc = 0;
    for (pass = 0; pass < NPASS; ++pass) {
        for (i = 0; i < NREC; ++i) {
            struct rec *r = &g_recs[i];
            /* Eight same-page loads off one base in one straight-line block. */
            long s = r->a + r->b + r->c + r->d + r->e + r->f + r->g + r->h;
            r->a = s;                  /* same-page store */
            r->h = s ^ (long) i;       /* second same-page store */
            acc = (acc << 1) - acc + s;
            g_sink = acc;
        }
    }

    printf("bltc-multiaccess: acc=%ld\n", acc);
    printf("bltc-multiaccess: PASS\n");
    return 0;
}
