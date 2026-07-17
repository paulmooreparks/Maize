/* fb_register.c -- maize-236 AC fixture, run UNDER quesOS.
 *
 * Proves the framebuffer registration syscall contract for a single process:
 *   - SYS_fb_geometry returns the configured width/height/format (320x200, format 1);
 *   - SYS_fb_register on a mapped buffer returns slot 0;
 *   - a second SYS_fb_register from the same process (no release) returns -EBUSY (D3);
 *   - SYS_fb_release then a fresh SYS_fb_register succeeds again.
 *
 * Output on success: "fb-register: PASS".
 */

int printf(const char *, ...);
long sys_fb_geometry(void *out);
long sys_fb_register(void *base);
long sys_fb_release(void);

static unsigned int fbbuf[64];   /* a mapped buffer to register (BSS page is mapped) */

int main(void) {
    unsigned int geo[3];
    long s1, s2, r, s3;

    geo[0] = 0; geo[1] = 0; geo[2] = 0;
    if (sys_fb_geometry(geo) != 0) { printf("fb-register: FAIL geometry-ret\n"); return 0; }
    if (geo[0] != 320 || geo[1] != 200 || geo[2] != 1) {
        printf("fb-register: FAIL geometry-vals\n");
        return 0;
    }

    s1 = sys_fb_register(fbbuf);
    if (s1 != 0) { printf("fb-register: FAIL first-register\n"); return 0; }

    s2 = sys_fb_register(fbbuf);
    if (s2 != -16) { printf("fb-register: FAIL not-ebusy\n"); return 0; }   /* -EBUSY */

    r = sys_fb_release();
    if (r != 0) { printf("fb-register: FAIL release\n"); return 0; }

    s3 = sys_fb_register(fbbuf);
    if (s3 != 0) { printf("fb-register: FAIL reregister\n"); return 0; }

    printf("fb-register: PASS\n");
    return 0;
}
