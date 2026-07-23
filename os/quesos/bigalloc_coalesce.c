/* bigalloc_coalesce.c -- maize-348 AC fixture, run UNDER quesOS.
 *
 * Proves rt's bigalloc-window coalescing fix (toolchain/rt/stdlib.c bigalloc_grow) at the
 * malloc()/realloc() layer, above the raw sys_bigalloc contract that os/quesos/bigalloc.c
 * already covers. Three legs, each isolated in a forked child so it gets a fresh 16 MiB
 * window (do_fork resets the child's bigalloc_next to the window base):
 *
 *   1) coalesce: two back-to-back window-backed grants, once freed, merge into ONE block a
 *      later allocation can use only via the merge. The window is first filled so no fresh
 *      sys_bigalloc grant can serve the request, and the request is larger than either freed
 *      grant alone, so success proves the two grants tiled and free_insert merged them. A
 *      full-span write and read-back proves one genuinely contiguous region.
 *
 *   2) growth: a realloc-growth loop mirroring demos/kilo/kilo.c:666's one-row-at-a-time
 *      E.row growth. The sbrk heap is pre-consumed first so the array spills into the window
 *      at a small size (as kilo's large image leaves it little sbrk headroom). On a PRE-FIX
 *      build the un-coalescable grants burn the window quadratically and realloc returns NULL
 *      well before the target (the negative control); on a POST-FIX build the loop reaches the
 *      target and a multi-MiB canary allocation afterward still succeeds (real headroom).
 *
 *   3) fork-cursor: the parent makes a real grant (so rt's g_bigalloc_end holds a genuine
 *      parent value), then forks. The child's first grant must NOT assume contiguity with the
 *      inherited, now-stale cursor; if it did, the block header would land outside the child's
 *      freshly mapped grant. A full-span write and read-back in the child proves it is bounded
 *      and writable.
 *
 * Compiled by the ordinary cc-maize.sh pipeline (stock .mzx); sys_bigalloc traps into quesOS's
 * cause-7 dispatcher. Output on success: bigalloc-coalesce: PASS
 */

#include "stdlib.h"
#include "stdio.h"
#include "syscall.h"

#define MB          (1024UL * 1024UL)
#define WINDOW      (16UL * MB)
#define BIGALLOC_LO 0x00400000UL   /* window base; a pointer at or above this is window-backed */

/* A window-backed payload sits at or above the window base; an sbrk block sits far below it
 * (region 0, capped at USER_BRK_MAX = 0x00100000). The gap makes the source unambiguous. */
static int is_big(void *p) {
    return (unsigned long)p >= BIGALLOC_LO;
}

/* Leg 1: direct coalescing proof. Returns 0 on pass, a nonzero code on failure.
 *
 * Two 2 MiB window grants (a, b) land at contiguous VAs. A large throwaway (t) is then
 * allocated to leave the window with too little headroom for a fresh 3 MiB grant, but the
 * window is NOT fully exhausted (that would drive the kernel's non-reclaiming bump allocator
 * into its ensure_l0 failure path, which is maize-349's concern, not this card's). Freeing a
 * and b merges them into one ~4 MiB block; a 3 MiB request then succeeds only through that
 * merge, since it exceeds either freed grant alone and the remaining window headroom. */
static int leg_coalesce(void) {
    unsigned long *cp;
    unsigned long i, n;
    void *a, *b, *t, *c;

    a = malloc(2 * MB);
    b = malloc(2 * MB);
    if (!a || !b)
        return 11;
    if (!is_big(a) || !is_big(b))
        return 12;   /* each must exceed the sbrk cap so it exercises the window path */

    /* Consume most of the remaining window (kept in use, never freed) so a fresh grant cannot
     * serve the 3 MiB request below, while leaving a safe margin off the window ceiling. */
    t = malloc(9 * MB);
    if (!t || !is_big(t))
        return 13;

    /* a and b were virtually contiguous grants; freed, the fix tiles them into one block. */
    free(a);
    free(b);

    /* 3 MiB is larger than either freed 2 MiB grant alone and larger than the window's
     * remaining headroom, so this can succeed only through the a+b merge. On a pre-fix build
     * the merge never forms and this returns NULL. */
    c = malloc(3 * MB);
    if (!c)
        return 14;

    cp = (unsigned long *)c;
    n = (3 * MB) / sizeof(unsigned long);
    for (i = 0; i < n; ++i)
        cp[i] = 0x1122334455667788UL ^ i;
    for (i = 0; i < n; ++i)
        if (cp[i] != (0x1122334455667788UL ^ i))
            return 15;
    return 0;
}

/* Leg 2: realloc-growth negative control. Returns 0 on pass, nonzero on failure. */
static int leg_realloc_growth(void) {
    unsigned long sz, target;
    void *buf, *nbuf, *canary, *warm;

    /* Pre-consume the sbrk heap so the growth array spills into the window at a small size,
     * mirroring kilo's environment. The spill is detected by address: sbrk blocks sit below
     * the window base. The sbrk-filling blocks are left allocated on purpose; only the single
     * window block we trip into is released. */
    while ((warm = malloc(8192)) != 0 && !is_big(warm)) {
        /* keep filling the sbrk heap */
    }
    if (warm && is_big(warm))
        free(warm);

    /* One unit at a time, mirroring demos/kilo/kilo.c:666's per-row realloc of E.row. */
    buf = 0;
    sz = 0;
    target = 300UL * 1024UL;
    for (;;) {
        sz += 256;
        nbuf = realloc(buf, sz);
        if (!nbuf) {
            /* Pre-fix: the window burns quadratically and this NULLs before target. */
            printf("bigalloc-coalesce: growth realloc NULL at sz=%lu (target=%lu)\n", sz, target);
            return 21;
        }
        buf = nbuf;
        ((char *)buf)[sz - 1] = (char)(sz & 0xFF);   /* touch the new tail */
        if (sz >= target)
            break;
    }

    /* Post-fix headroom check: a multi-MiB canary must still succeed, proving the fix leaves
     * real window headroom rather than a narrow escape. */
    canary = malloc(4 * MB);
    if (!canary)
        return 22;
    free(canary);
    free(buf);
    return 0;
}

/* Leg 3: post-fork cursor safety. Returns 0 on pass, nonzero on failure.
 *
 * The parent makes one window grant so rt's g_bigalloc_end holds a genuine parent value that
 * the child inherits stale through fork's eager copy, then forks and has the child do its own
 * first window allocation. My fix must make that child grant fall back to the independently
 * padded construction (the reset window base never equals the stale cursor + HDR) rather than
 * place a header at the inherited cursor, which would land outside the child's fresh grant.
 *
 * One wrinkle forces the parent's request size. fork EXCLUDES the bigalloc window from the
 * child's address space (the window is re-based, not eager-copied; os/quesos/quesos.c), so any
 * window free block the child inherited in rt's free list would be an UNMAPPED VA that the
 * child's first find_fit() would fault on while walking the list. That fault is an orthogonal
 * fork / free-list interaction independent of this card's fix (it reproduces on pre-fix rt
 * too) and out of scope here. To test only the cursor fallback, the parent leaves rt's free
 * list EMPTY before forking: it requests a size whose grant fits exactly, with no free
 * remainder to split off. The size below is derived from toolchain/rt/stdlib.c's allocator
 * geometry so grow_amount == block_total exactly; PARENT_EXACT_PAYLOAD also stays above the
 * quesOS sbrk cap (USER_BRK_MAX, ~1 MiB) so the grant is genuinely window-backed. */
#define A_ALIGN 16UL
#define A_HDR    8UL
#define A_PAGE  0x1000UL
/* Block total == 512 pages minus one ALIGN, so total + ALIGN is page-aligned and
 * bigalloc_grow's page-rounded grant equals the block exactly (zero remainder, empty list). */
#define PARENT_EXACT_TOTAL   (512UL * A_PAGE - A_ALIGN)
#define PARENT_EXACT_PAYLOAD (PARENT_EXACT_TOTAL - A_HDR)

static int leg_fork_cursor(void) {
    unsigned char *cb;
    unsigned long i, n;
    void *pa, *ca;
    long pid;
    int status, rc;

    pa = malloc(PARENT_EXACT_PAYLOAD);
    if (!pa || !is_big(pa))
        return 31;
    ((char *)pa)[0] = 0x5A;   /* keep pa in use; do not free it (freeing would refill the list) */

    pid = sys_fork();
    if (pid == 0) {
        /* Child: bigalloc_next was reset to the window base, but g_bigalloc_end is the stale
         * inherited value. The first grant must fall back to the independently padded block
         * rather than trust the cursor; otherwise the header lands outside the fresh grant.
         * The child terminates with _exit so control never returns into the parent's flow. */
        ca = malloc(2 * MB);
        if (!ca)
            _exit(32);
        if (!is_big(ca) || (unsigned long)ca >= BIGALLOC_LO + WINDOW)
            _exit(33);   /* must be a fresh, correctly bounded window block */
        cb = (unsigned char *)ca;
        n = 2 * MB;
        for (i = 0; i < n; ++i)
            cb[i] = (unsigned char)(i & 0xFF);
        for (i = 0; i < n; ++i)
            if (cb[i] != (unsigned char)(i & 0xFF))
                _exit(34);   /* a mis-placed header would have corrupted this span */
        _exit(0);
    }

    status = 0;
    if (sys_wait4(pid, &status, 0, 0) != pid)
        return 35;
    rc = (status >> 8) & 0xFF;
    return rc;   /* the child's exit code, 0 when its post-fork allocation checked out */
}

int main(void) {
    long pid;
    int status, rc;

    /* Unbuffered so each progress line reaches the console immediately; a fault in any leg
     * then leaves the transcript pointing at the last line printed rather than losing a
     * full-buffer's worth of output when the VM aborts (stdout is buffered by default). */
    setvbuf(stdout, (char *)0, _IONBF, 0);

    printf("bigalloc-coalesce: start\n");

    /* Growth negative control runs first, in its own forked child (fresh window). It is the
     * leg that most directly separates pre-fix from post-fix: on a pre-fix build it returns
     * NULL before target (the quadratic window burn) and this leg fails here; on a post-fix
     * build it reaches target with headroom to spare. Running it first makes a pre-fix
     * transcript show the growth NULL directly rather than short-circuiting on leg 2. */
    printf("bigalloc-coalesce: leg1 growth...\n");
    pid = sys_fork();
    if (pid == 0)
        _exit(leg_realloc_growth());
    status = 0;
    if (sys_wait4(pid, &status, 0, 0) != pid) {
        printf("bigalloc-coalesce: FAIL wait-growth\n");
        return 0;
    }
    rc = (status >> 8) & 0xFF;
    if (rc != 0) {
        printf("bigalloc-coalesce: FAIL growth rc=%d\n", rc);
        return 0;
    }

    /* Direct coalescing proof, in its own forked child (its large throwaway is kept in use). */
    printf("bigalloc-coalesce: leg2 coalesce...\n");
    pid = sys_fork();
    if (pid == 0)
        _exit(leg_coalesce());
    status = 0;
    if (sys_wait4(pid, &status, 0, 0) != pid) {
        printf("bigalloc-coalesce: FAIL wait-coalesce\n");
        return 0;
    }
    rc = (status >> 8) & 0xFF;
    if (rc != 0) {
        printf("bigalloc-coalesce: FAIL coalesce rc=%d\n", rc);
        return 0;
    }

    /* Leg 3 contains its own parent-then-fork structure, so it runs in this process. */
    printf("bigalloc-coalesce: leg3 fork-cursor...\n");
    rc = leg_fork_cursor();
    if (rc != 0) {
        printf("bigalloc-coalesce: FAIL fork-cursor rc=%d\n", rc);
        return 0;
    }

    printf("bigalloc-coalesce: PASS\n");
    return 0;
}
