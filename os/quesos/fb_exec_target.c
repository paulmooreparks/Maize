/* fb_exec_target.c -- maize-236 AC fixture (exec target), run UNDER quesOS.
 *
 * The image fb_exec_launch execs into. It registers a framebuffer and must get slot 0,
 * proving the launcher's registration was released across the exec (Decision D5). See
 * fb_exec_launch.c.
 *
 * Output on success: "fb-exec: PASS".
 */

int printf(const char *, ...);
long sys_fb_register(void *base);

static unsigned int fbbuf[64];

int main(void) {
    long s = sys_fb_register(fbbuf);
    if (s == 0) { printf("fb-exec: PASS\n"); }
    else { printf("fb-exec: FAIL target-slot\n"); }
    return 0;
}
