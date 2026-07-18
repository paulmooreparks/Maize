/* bulk_bounds.c -- maize-247 AC fixture (AC 9350), run UNDER quesOS.
 *
 * Proves a bulk syscall whose range includes at least one UNMAPPED page returns -EFAULT (14)
 * and touches NO byte: a sentinel placed immediately before the unmapped boundary is verified
 * unchanged after the rejected call (quesOS validates the whole range before any native call,
 * so there is no partial write).
 *
 * Layout: grow the heap so [base, base+0x2000) is two mapped pages; the page at base+0x2000
 * is past the break and therefore unmapped. A range [base+0x1000, base+0x3000) then has its
 * first page mapped and its second page unmapped, and the sentinel at base+0x1FFF (last byte
 * of the mapped page) must survive.
 * Output on success: bulk-bounds: PASS
 */
int  printf(const char *, ...);
long sys_brk(unsigned long addr);
long sys_bulk_copy(void *dst, const void *src, unsigned long n);
long sys_bulk_set(void *dst, int c, unsigned long n);

int main(void) {
    unsigned long cur  = (unsigned long)sys_brk(0);
    unsigned long base = (cur + 0xFFFUL) & ~0xFFFUL;
    unsigned char *good;      /* [base, base+0x2000): two fully-mapped pages */
    unsigned char *edge;      /* [base+0x1000, base+0x3000): mapped page then unmapped page */
    unsigned char *sentinel;  /* last byte of the mapped page in the edge range */
    long rv;

    if (sys_brk(base + 0x2000UL) != (long)(base + 0x2000UL)) {
        printf("bulk-bounds: FAIL brk\n");
        return 0;
    }
    good     = (unsigned char *)base;
    edge     = (unsigned char *)(base + 0x1000UL);
    sentinel = (unsigned char *)(base + 0x2000UL - 1UL);
    *sentinel = 0x7E;

    /* sys_bulk_set whose range crosses into the unmapped page: -EFAULT, sentinel untouched. */
    rv = sys_bulk_set(edge, 0x33, 0x2000UL);
    if (rv != -14) { printf("bulk-bounds: FAIL set rv=%d\n", (int)rv); return 0; }
    if (*sentinel != 0x7E) { printf("bulk-bounds: FAIL set-sentinel\n"); return 0; }

    /* sys_bulk_copy with dst crossing the unmapped boundary (src = the mapped good buffer):
     * -EFAULT, sentinel (in dst's first, mapped page) untouched. */
    rv = sys_bulk_copy(edge, good, 0x2000UL);
    if (rv != -14) { printf("bulk-bounds: FAIL copy rv=%d\n", (int)rv); return 0; }
    if (*sentinel != 0x7E) { printf("bulk-bounds: FAIL copy-sentinel\n"); return 0; }

    printf("bulk-bounds: PASS\n");
    return 0;
}
