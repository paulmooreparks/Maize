/* fb_present.c -- maize-251 AC fixture (AC 9316), run UNDER quesOS.
 *
 * Proves sys_fb_present's ($FD) contract:
 *   - a process holding no fb registration gets -EBADF (mirrors do_fb_release's guard);
 *   - after sys_fb_mmap + sys_fb_register, sys_fb_present returns 0 with no PANIC/crash --
 *     the trailing PASS print proves the VM survived the present (the device copied the
 *     slot's surface into its capture buffer with no host out-of-bounds).
 *
 * Uses a real fb-mmap'd, physically-contiguous buffer (not a one-page dummy) so the present
 * exercises the same path DOOM's DG_DrawFrame drives. Output on success: fb-present: PASS
 */

int  printf(const char *, ...);
long sys_fb_mmap(void);
long sys_fb_register(void *base);
long sys_fb_present(void);

int main(void) {
    long va, s, r;
    unsigned int *fb;

    /* no registration yet -> -EBADF (9) */
    if (sys_fb_present() != -9) { printf("fb-present: FAIL not-ebadf\n"); return 0; }

    va = sys_fb_mmap();
    if (va < 0) { printf("fb-present: FAIL mmap\n"); return 0; }
    fb = (unsigned int *)(unsigned long)va;
    fb[0] = 0xFF112233u;   /* something for the present to copy */

    s = sys_fb_register(fb);
    if (s < 0) { printf("fb-present: FAIL register\n"); return 0; }

    /* present the registered slot: 0 and no crash. */
    r = sys_fb_present();
    if (r != 0) { printf("fb-present: FAIL present rc=%ld\n", r); return 0; }

    /* a second present is idempotent (still 0, still no crash). */
    if (sys_fb_present() != 0) { printf("fb-present: FAIL present2\n"); return 0; }

    printf("fb-present: PASS\n");
    return 0;
}
