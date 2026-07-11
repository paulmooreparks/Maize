/* maize-101 bug #3: short-circuit `&&` in a loop condition, a pointer-selecting
 * ternary `?:`, and a two-cursor exchange over an intrusive list, the natural
 * forms maize-76 had to replace with single-condition helper loops + if/else.
 *
 * rega resolves loop-carried phis into edge copies; when two live pointers
 * exchange each iteration the moves form a register cycle, which rega's pmgen
 * lowers to Oswap. The maize overlay emitter had no Oswap case and die()'d with
 *   maize emit: unimplemented op 'swap'
 * The overlay now lowers Oswap to maize `XCHG ra rb` ($E0) on the two physical
 * register operands at whole-register width (this fixture emits real XCHGs;
 * reverting the emit.c change reintroduces the die).
 *
 * Node pointers are routed through the non-inlined opaque() barrier (the capstone
 * idiom, capstone.c) so they are held live in registers rather than being
 * rematerialized from an address-of-local; that keeps this fixture squarely on
 * the Oswap/XCHG path under test and off the separate address-rematerialization
 * codegen path. Stdout depends on the walk, the ternary, and the exchange all
 * resolving.
 */
int puts(const char *);

struct node { int val; struct node *next; };

/* Not inlined by cproc/qbe, so the returned pointer is opaque to the folder and
 * the rematerializer (capstone.c documents this barrier). */
static struct node *opaque(struct node *p) { return p; }

int main(void) {
	struct node n0, n1, n2;
	n0.val = 1; n0.next = &n1;
	n1.val = 2; n1.next = &n2;
	n2.val = 3; n2.next = 0;

	/* Natural walk: short-circuit && in the loop condition. */
	struct node *p = opaque(&n0);
	while (p && p->next)
		p = p->next;                 /* p -> last node (n2) */

	/* Pointer-selecting ternary (phi over pointers). */
	struct node *tail = p->next ? p->next : p;   /* == p == &n2 */

	/* Two cursors exchanged each step: the loop-carried register cycle that
	 * lowers to XCHG. An odd trip count leaves them swapped. */
	struct node *lo = opaque(&n0);       /* &n0 (val 1) */
	struct node *hi = tail;              /* &n2 (val 3) */
	int steps = n0.val + n1.val;         /* 3 (odd) -> ends swapped */
	while (steps-- > 0) {
		struct node *t = lo;
		lo = hi;
		hi = t;
	}

	if (lo->val == 3 && hi->val == 1)    /* lo=&n2, hi=&n0 after an odd count */
		puts("freelist: PASS");
	else
		puts("freelist: FAIL");
	return 0;
}
