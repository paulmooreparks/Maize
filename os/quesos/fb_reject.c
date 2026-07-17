/* fb_reject.c -- maize-236 AC fixture, run UNDER quesOS on a display-less view.
 *
 * Booted with --fb-no-display (display_available_=false, stop_on_claim_=false), the device
 * rejects the claim: SYS_fb_register returns -ENODEV and quesOS itself keeps running (only
 * the individual request failed; the VM is NOT powered off, evolving maize-221 from
 * whole-VM halt to per-exec rejection). The trailing PASS print proves the VM survived.
 *
 * Output on success: "fb-reject: PASS".
 */

int printf(const char *, ...);
long sys_fb_register(void *base);

static unsigned int fbbuf[64];

int main(void) {
    long s = sys_fb_register(fbbuf);
    if (s == -19) {   /* -ENODEV: the device rejected the display-less claim */
        printf("fb-reject: PASS\n");
    } else {
        printf("fb-reject: FAIL\n");
    }
    return 0;
}
