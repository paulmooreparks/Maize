/* bulk_kernel_range.c -- maize-247 AC fixture (AC 9351), run UNDER quesOS.
 *
 * Proves the PTE_U gate (Decision 6): a bulk syscall naming a kernel-owned VA (PTE_V=1,
 * PTE_U=0) as either src or dst returns -EFAULT (14) rather than succeeding. Without this
 * check a crafted user VA could drive a native WRITE into quesOS's own code/data, since the
 * kernel-identity range shares region 0's L0 table with the user pages. 0x100000 is the
 * quesOS image base (mapped kernel, PTE_U=0 in every process table).
 *
 * A mapped user sentinel is verified unchanged after a rejected copy, confirming the range
 * is rejected before any byte moves.
 * Output on success: bulk-kernel: PASS
 */
int  printf(const char *, ...);
long sys_brk(unsigned long addr);
long sys_bulk_copy(void *dst, const void *src, unsigned long n);
long sys_bulk_set(void *dst, int c, unsigned long n);

#define KERNEL_VA 0x100000UL   /* quesOS image base: PTE_V=1, PTE_U=0 (kernel-owned) */

int main(void) {
    unsigned long cur  = (unsigned long)sys_brk(0);
    unsigned long base = (cur + 0xFFFUL) & ~0xFFFUL;
    unsigned char *good;
    long rv;

    if (sys_brk(base + 0x1000UL) != (long)(base + 0x1000UL)) {
        printf("bulk-kernel: FAIL brk\n");
        return 0;
    }
    good = (unsigned char *)base;
    good[0] = 0x5A;   /* user sentinel: must survive a rejected copy */

    /* sys_bulk_set targeting the kernel image page: PTE_U=0 -> -EFAULT. */
    rv = sys_bulk_set((void *)KERNEL_VA, 0xFF, 0x100UL);
    if (rv != -14) { printf("bulk-kernel: FAIL set rv=%d\n", (int)rv); return 0; }

    /* sys_bulk_copy with the kernel page as SRC (dst is the mapped user page): -EFAULT, and
     * the user sentinel is untouched. */
    rv = sys_bulk_copy(good, (const void *)KERNEL_VA, 0x100UL);
    if (rv != -14) { printf("bulk-kernel: FAIL copy-src rv=%d\n", (int)rv); return 0; }
    if (good[0] != 0x5A) { printf("bulk-kernel: FAIL sentinel\n"); return 0; }

    /* sys_bulk_copy with the kernel page as DST (src is the mapped user page): also -EFAULT
     * (the write that would corrupt quesOS is rejected before it is issued). */
    rv = sys_bulk_copy((void *)KERNEL_VA, good, 0x100UL);
    if (rv != -14) { printf("bulk-kernel: FAIL copy-dst rv=%d\n", (int)rv); return 0; }

    printf("bulk-kernel: PASS\n");
    return 0;
}
