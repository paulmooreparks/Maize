/* pagefault_toplevel.c -- maize-369 AC fixture, run UNDER quesOS.
 *
 * A TOP-LEVEL worklist process (parent 0, i.e. init) stores a byte through a wild
 * pointer and dies. This is the arm the operator's DOOM session takes: reap_tail's
 * parent-0 branch frees the PCB outright (P_FREE) rather than leaving a zombie for a
 * wait, which is a different path from the forked-child arm pagefault_kill.c covers.
 *
 * VA 0 is in L0 slot 0, which build_address_space never maps (it maps the trap page at
 * slot 1 and the image at 256..511, and loaded segments start at or above 0x2000), so
 * the fault is present=0, access=store, user=1 and CR2 reads 0x0C.
 *
 * No output on success: the process dies before it can print, and quesOS's own
 * "[quesos] page fault:" line is the observable. The FAIL marker below only appears if
 * the store somehow did not fault, which would mean the fixture stopped testing anything.
 *
 * The wild pointer is a FILE-SCOPE object, not a local `volatile`: the pinned cproc-qbe
 * cannot lower a volatile store (os/quesos/kill_launch.c:32-36), and cproc does no
 * constant folding, so a store through a global pointer is emitted as written.
 *
 * The stored BYTE also comes from a file-scope object rather than being written as a
 * literal, which is a second, separate toolchain accommodation. Writing the obvious
 * `*g_wild = 1;` dies in the Maize QBE backend with "maize emit: unsupported memory
 * address" (qbe-maize/emit.c:152, reached from memaddrreg); routing the value through a
 * variable emits the same store and compiles. That limitation is pre-existing and has
 * nothing to do with this card, so the fixtures route around it rather than chase it.
 */

int printf(const char *, ...);

char *g_wild;
char  g_byte;

int main(void) {
    g_wild = (char *)0;
    g_byte = 1;
    *g_wild = g_byte;            /* cause 8: store, present=0, user=1 (CR2 = 0x0C) */
    printf("pagefault-toplevel: FAIL survived\n");
    return 0;
}
