/* pagefault_kernel.c -- maize-369 AC fixture, run UNDER quesOS.
 *
 * Proves the SUPERVISOR arm of the cause-8 handler: a fault taken with CR2's USER bit
 * clear is a kernel panic, printed in a visibly different form and followed by a
 * poweroff, not a process kill.
 *
 * sys_open's first act is copy_user_path, which walks the user path string by direct
 * dereference through the live translation while running supervisor. Handing it VA
 * 0x10000000 therefore faults in kernel context: L1 index 128 is linked by no address
 * space (region 0 is user, region 1 the fb window, regions 2..9 bigalloc, regions 10..41
 * the identity-mapped pool, and the stack region is 0x05400000), so the VA is unmapped
 * however far this fixture's heap grew. Expect present=0, access=load, user=0, CR2 = 0x02.
 *
 * The VM halts through quesos_poweroff, so nothing after the call runs. The FAIL marker
 * only appears if sys_open somehow returned, which would mean the fixture stopped
 * testing anything.
 *
 * The bad pointer is a FILE-SCOPE object for the same reason the other pagefault_*
 * fixtures use one: the pinned cproc-qbe cannot lower a volatile store
 * (os/quesos/kill_launch.c:32-36).
 */

int printf(const char *, ...);
long sys_open(const char *path, int flags, int mode);

const char *g_bad_path;

int main(void) {
    g_bad_path = (const char *)0x10000000;
    sys_open(g_bad_path, 0, 0);   /* copy_user_path derefs it supervisor: cause 8, user=0 */
    printf("pagefault-kernel: FAIL survived\n");
    return 0;
}
