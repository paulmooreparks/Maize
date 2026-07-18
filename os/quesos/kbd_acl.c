/* kbd_acl.c -- maize-251 AC fixture (AC 9315), run UNDER quesOS with --input=keyboard and a
 * single Set-1 scancode byte piped on stdin.
 *
 * Proves sys_kbd_read's ($FE) ACL and status semantics with ONE injected scancode, all three
 * legs deterministic (the scancode stays latched across a non-owner read because -EACCES does
 * not consume it):
 *   1. NON-OWNER, key pending -> -EACCES. Register then release so this process owns no active
 *      slot; wait for the injected scancode to latch (spin past -EAGAIN), then read -> -EACCES.
 *      The scancode is NOT consumed (the real owner still gets it).
 *   2. OWNER, key pending -> the scancode. Re-register (own the active slot again) and read the
 *      still-latched scancode (0-255).
 *   3. OWNER, none pending -> -EAGAIN. Read again after consuming the only injected key.
 *
 * Mirrors the raw port semantics kbd_status/kbd_read already have in the bare-VM shim, but
 * mediated + gated by quesOS. Output on success: kbd-acl: PASS
 */

int  printf(const char *, ...);
long sys_fb_register(void *base);
long sys_fb_release(void);
long sys_kbd_read(void);

static unsigned int fbbuf[64];   /* a mapped buffer to register (BSS page is mapped) */

int main(void) {
    long r;

    /* Own then release the active fb slot, leaving this process a NON-owner (fb_slot = -1). */
    if (sys_fb_register(fbbuf) < 0) { printf("kbd-acl: FAIL register1\n"); return 0; }
    if (sys_fb_release() != 0)      { printf("kbd-acl: FAIL release\n");   return 0; }

    /* Leg 1: as a non-owner, wait for the scancode to latch (-EAGAIN while none pending),
     * then the ungated read is refused with -EACCES WITHOUT consuming the scancode. */
    for (;;) {
        r = sys_kbd_read();
        if (r == -11) { continue; }   /* -EAGAIN: not latched yet */
        break;
    }
    if (r != -13) { printf("kbd-acl: FAIL not-eacces r=%ld\n", r); return 0; }

    /* Leg 2: become the active owner again; the SAME still-latched scancode is now readable. */
    if (sys_fb_register(fbbuf) < 0) { printf("kbd-acl: FAIL register2\n"); return 0; }
    r = sys_kbd_read();
    if (r < 0 || r > 255) { printf("kbd-acl: FAIL owner-read r=%ld\n", r); return 0; }

    /* Leg 3: the queue is now drained (only one byte injected), so the owner sees -EAGAIN. */
    if (sys_kbd_read() != -11) { printf("kbd-acl: FAIL not-eagain\n"); return 0; }

    printf("kbd-acl: PASS\n");
    return 0;
}
