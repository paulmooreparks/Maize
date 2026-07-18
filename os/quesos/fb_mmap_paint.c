/* fb_mmap_paint.c -- maize-238 Family C AC fixture (AC 9183), run UNDER quesOS.
 *
 * The first fixture to prove physical contiguity end to end (unlike the existing one-page
 * dummy-buffer fixtures): sys_fb_mmap() returns a VA; the process fills the ENTIRE buffer
 * (all ~63 pages for 320x200 XRGB8888, not just page 0) with a known non-trivial pattern;
 * sys_fb_register(va) succeeds (its full-range contiguity check passes across every page);
 * and reading the whole buffer back confirms every page is mapped to a real, distinct
 * frame holding what was written -- the contiguous physical range the dumb scanout device
 * reads linearly. Output on success: fb-mmap-paint: PASS
 */
int  printf(const char *, ...);
long sys_fb_mmap(void);
long sys_fb_register(void *base);
long sys_fb_geometry(void *out);

int main(void) {
    unsigned int geo[3];
    unsigned long npix, i;
    long va, s;
    unsigned int *fb;

    if (sys_fb_geometry(geo) != 0) { printf("fb-mmap-paint: FAIL geometry\n"); return 0; }
    npix = (unsigned long)geo[0] * (unsigned long)geo[1];   /* 320*200 = 64000 */

    va = sys_fb_mmap();
    if (va < 0) { printf("fb-mmap-paint: FAIL mmap\n"); return 0; }
    fb = (unsigned int *)(unsigned long)va;

    /* Fill EVERY pixel with a non-trivial, position-dependent pattern spanning all pages. */
    for (i = 0; i < npix; ++i) {
        fb[i] = (unsigned int)(0xFF000000u | ((unsigned int)i * 2654435761u));
    }

    s = sys_fb_register(fb);   /* validates full-range physical contiguity */
    if (s < 0) { printf("fb-mmap-paint: FAIL register\n"); return 0; }

    /* Read the WHOLE buffer back: proves every page holds its written value (contiguous,
     * distinct real frames end to end). */
    for (i = 0; i < npix; ++i) {
        unsigned int want = (unsigned int)(0xFF000000u | ((unsigned int)i * 2654435761u));
        if (fb[i] != want) { printf("fb-mmap-paint: FAIL readback\n"); return 0; }
    }

    printf("fb-mmap-paint: PASS\n");
    return 0;
}
