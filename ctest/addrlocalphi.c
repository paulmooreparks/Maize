/* maize-103: an `&local` (address of a stack local) carried DIRECTLY as a
 * loop-carried phi argument, WITHOUT the opaque() barrier ctest/freelist.c uses.
 *
 * This is the deliberate inverse of freelist.c. freelist routes its node
 * pointers through the non-inlined opaque() helper (the capstone.c idiom) so the
 * live value is a call result held in a register: that keeps it on the Oswap/XCHG
 * path and OFF the address-rematerialization path. Here there is no barrier: `p`
 * is initialized to the address of a local and walked across a `while (p &&
 * p->next)` loop, so cproc/qbe emit a phi whose entry-edge argument is the alloc
 * temp of the local directly.
 *
 * The Maize target isel (toolchain/qbe-maize/isel.c) assigns a frame slot to each
 * Oalloc result and nops the alloc, leaving `&local` as a slot-temp with no
 * defining instruction. fixarg materializes such a temp into an Oaddr (LEA) when
 * it appears as a straight-line operand, but before maize-103 the block loop never
 * visited SUCCESSOR PHI ARGUMENTS (amd64/isel.c and arm64/isel.c both do). So the
 * alloc temp survived to rega, which resolved the phi edge as a plain slot MOVE of
 * the local's CONTENTS instead of a LEA of its ADDRESS: a silent wrong answer (or
 * a hard die in emit, which has no RSlot operand case). The fix adds the missing
 * successor-phi-argument fixarg pass to maize_isel; reverting it reintroduces the
 * defect and this fixture fails.
 *
 * Distinctive field values make a coincidental pass from garbage slot contents
 * astronomically unlikely: a wrong address read would derail the walk immediately.
 */
int puts(const char *);

struct node { int val; struct node *next; };

int main(void) {
	struct node n0, n1, n2;
	n0.val = 101; n0.next = &n1;
	n1.val = 202; n1.next = &n2;
	n2.val = 303; n2.next = 0;

	/* Loop-carried &local pointer, no opaque() barrier. The entry-edge phi
	 * argument for `p` is the alloc temp of n0 taken directly by address. */
	struct node *p = &n0;
	while (p && p->next)
		p = p->next;                 /* p -> &n2 (the last node, val 303) */

	/* Reach a field through the walked address-of-local pointer, plus sum the
	 * chain from a second &local cursor to exercise a second loop-carried
	 * address-of-local phi. */
	int sum = 0;
	struct node *q = &n0;
	while (q) {
		sum += q->val;               /* 101 + 202 + 303 == 606 */
		q = q->next;
	}

	if (p->val == 303 && sum == 606)
		puts("addrlocalphi: PASS");
	else
		puts("addrlocalphi: FAIL");
	return 0;
}
