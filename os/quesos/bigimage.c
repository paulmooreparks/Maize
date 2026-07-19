/* bigimage.c -- maize-251 addendum: an intentionally OVERSIZED guest, exec'd by loader_guard.c.
 *
 * Its static BSS alone (~1.13 MiB) pushes the image's end-of-data VA well past USER_BRK_MAX
 * (0x00100000), so quesOS's load_segments segment-fits-USER_BRK_MAX guard must reject it BEFORE
 * mapping the oversized segment. It is built into /progs but never placed on the worklist and
 * must never actually run; if it prints, the guard failed to fire.
 */
int printf(const char *, ...);

static unsigned char huge[0x120000];   /* ~1.13 MiB BSS: image end > USER_BRK_MAX */

int main(void) {
    huge[0] = 1;
    huge[0x11FFFF] = 2;
    printf("bigimage: SHOULD-NOT-RUN %d\n", (int)(huge[0] + huge[0x11FFFF]));
    return 0;
}
