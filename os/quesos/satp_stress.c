/* satp_stress.c -- maize-346 AC 9830 fixture, run UNDER quesOS (paged / Sv48).
 *
 * Proves the paged JIT's cross-page + indirect-transfer probe is SATP-safe. quesOS gives
 * each process its own page table, so the round-robin scheduler forces SATP context
 * switches while paged probe entries and cross-page transfers are hot. Several children
 * each run a long hot loop of indirect (function-pointer) dispatches, which the JIT tiers
 * up and routes through the physical-keyed paged probe tail. Each child folds its work
 * into an 8-bit hash and returns it as its exit status; the parent reaps in pid order and
 * prints every hash plus a summary, so stdout is fully deterministic regardless of the
 * interleaving. The oracle is external: run under plain --jit and under the interpreter,
 * the two stdout streams must be byte-identical. A probe entry that pointed at the wrong
 * physical block after an SATP switch would compute a different hash and diverge the output
 * (--jit-check cannot catch this: it disables both the probe and chaining).
 */

int printf(const char *, ...);
long sys_fork(void);
long sys_wait4(long pid, int *status, long options, long rusage);

/* Four distinct hash steps behind a function-pointer table, so the hot loop's dispatch
   compiles to an indirect CALL through a register (and a RET back), the exact transfers
   Lever 2 routes through the paged probe. Kept as separate external-linkage functions so
   the compiler cannot fold them into the caller. */
long step_add(long a, long k) { return a + k * 2654435761L; }
long step_xor(long a, long k) { return a ^ (k + 0x9E3779B1L); }
long step_mix(long a, long k) { return (a << 5) - a + k; }
long step_rot(long a, long k) { return (a * 1000003L) ^ (k << 3); }

typedef long (*step_fn)(long, long);
step_fn g_steps[4] = { step_add, step_xor, step_mix, step_rot };

#define NCHILD 6
#define NITER  120000

/* External linkage so the accumulate loop is never optimized away. */
long g_child_sink;

long child_hash(long seed) {
    long acc = seed;
    long k;
    for (k = 0; k < NITER; ++k) {
        step_fn f = g_steps[(acc + k) & 3];   /* indirect dispatch: CALL/RET through the table */
        acc = f(acc, k);
        g_child_sink = acc;
    }
    return acc & 0xFF;
}

int main(void) {
    long pids[NCHILD];
    int i;

    for (i = 0; i < NCHILD; ++i) {
        long p = sys_fork();
        if (p == 0) {
            return (int) child_hash((long) i * 2246822519L + 12345L);
        }
        pids[i] = p;
    }

    int ok = 1;
    for (i = 0; i < NCHILD; ++i) {
        int st = 0;
        long w = sys_wait4(pids[i], &st, 0, 0);
        int h = (st >> 8) & 0xFF;
        if (w != pids[i]) { ok = 0; }
        printf("satp-child %d: %d\n", i, h);
    }
    printf(ok ? "satp-stress: PASS\n" : "satp-stress: FAIL\n");
    return 0;
}
