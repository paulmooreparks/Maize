/* palette_blit_guard.c -- maize-251 addendum NEGATIVE fixture, run UNDER quesOS.
 *
 * Proves do_palette_blit's ($F3) up-front validation (Convention counterexamples Entry 11):
 * a forwarded memory syscall must reject a crafted/invalid guest VA BEFORE any translation,
 * because user_pa / as_read* / as_write* do NO PTE_U/PTE_V gate. Each of the following returns
 * -EFAULT (14) with NO guest/kernel memory touched (sentinel-checked), and the VM keeps running:
 *   - a kernel-owned dst VA (the quesOS image base 0x00100000, PTE_U=0),
 *   - an unmapped dst VA (a region with no L0),
 *   - a kernel-owned SRC VA and a kernel-owned LUT VA (proves ALL three ranges are validated),
 *   - a base+len wrap (dst near the top of the 64-bit space),
 *   - an npixels*4 overflow.
 * A rejected call with a VALID dst but an invalid src/lut must leave dst untouched (the
 * validate-before-write invariant). A trailing positive control confirms a valid blit still
 * works. Output on success: palette-guard: PASS
 */

int  printf(const char *, ...);
long sys_palette_blit(void *dst, const void *src, const unsigned int *lut, unsigned long npixels);

static unsigned int  g_lut[256];
static unsigned char g_src[64];
static unsigned int  g_dst[64];

#define KERNEL_VA ((void *)0x00100000ul)   /* quesOS image base: PTE_V, PTE_U=0 */
#define UNMAPPED_VA ((void *)0x00300000ul) /* region 1, no L0 until fb_mmap: unmapped */
#define EFAULT (-14)

int main(void) {
    long r;

    g_dst[0] = 0xDEADBEEFu;   /* sentinel in our own VALID buffer; no rejected call may touch it */

    /* Kernel-owned dst -> -EFAULT (PTE_U=0). */
    r = sys_palette_blit(KERNEL_VA, g_src, g_lut, 16);
    if (r != EFAULT) { printf("palette-guard: FAIL kernel-dst r=%ld\n", r); return 0; }

    /* Unmapped dst (no L0 for its region) -> -EFAULT. */
    r = sys_palette_blit(UNMAPPED_VA, g_src, g_lut, 16);
    if (r != EFAULT) { printf("palette-guard: FAIL unmapped-dst r=%ld\n", r); return 0; }

    /* Kernel-owned SRC, valid dst -> -EFAULT before any write (dst sentinel must survive). */
    r = sys_palette_blit(g_dst, KERNEL_VA, g_lut, 16);
    if (r != EFAULT) { printf("palette-guard: FAIL kernel-src r=%ld\n", r); return 0; }

    /* Kernel-owned LUT, valid dst/src -> -EFAULT before any write. */
    r = sys_palette_blit(g_dst, g_src, (const unsigned int *)KERNEL_VA, 16);
    if (r != EFAULT) { printf("palette-guard: FAIL kernel-lut r=%ld\n", r); return 0; }

    /* base+len wrap (dst near the 64-bit top) -> -EFAULT, no OOB write. */
    r = sys_palette_blit((void *)0xFFFFFFFFFFFFF000ul, g_src, g_lut, 4096);
    if (r != EFAULT) { printf("palette-guard: FAIL wrap r=%ld\n", r); return 0; }

    /* npixels*4 overflow -> -EFAULT. */
    r = sys_palette_blit(g_dst, g_src, g_lut, 0x4000000000000000ul);
    if (r != EFAULT) { printf("palette-guard: FAIL npix-overflow r=%ld\n", r); return 0; }

    /* No corruption: our valid dst sentinel is untouched by every rejected call. */
    if (g_dst[0] != 0xDEADBEEFu) { printf("palette-guard: FAIL dst-corrupted\n"); return 0; }

    /* Positive control: a fully-valid blit still succeeds and produces dst[i] = lut[src[i]]. */
    {
        int i;
        for (i = 0; i < 256; ++i) { g_lut[i] = 0xFF000000u | (unsigned)i; }
        for (i = 0; i < 64; ++i)  { g_src[i] = (unsigned char)i; }
        r = sys_palette_blit(g_dst, g_src, g_lut, 64);
        if (r != 64) { printf("palette-guard: FAIL valid-blit r=%ld\n", r); return 0; }
        if (g_dst[0] != (0xFF000000u | 0u) || g_dst[63] != (0xFF000000u | 63u)) {
            printf("palette-guard: FAIL valid-result\n"); return 0;
        }
    }

    printf("palette-guard: PASS\n");
    return 0;
}
