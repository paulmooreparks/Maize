/* maize-101 bug #1: a void-returning call taking >=1 argument (the free() shape).
 *
 * Before the fix, `qbe -t maize` aborted with
 *   spill.c:431: Assertion 't >= Tmp0 && "dead reg"' failed
 * on any `void sink(void*); ... sink(p);`: a void call emitted no post-call
 * register copy, so spill.c's dopm never applied the target's argument-register
 * liveness to the Ocall, and the CP arg -> R0 def looked dead. The overlay
 * selcall now always emits the RV result-copy (a dead `copy R, RV` that keeps
 * dopm engaged and is eliminated by rega), mirroring amd64 sysv.
 *
 * sink() writes through its pointer argument, so the argument provably reached
 * the callee: the observable stdout depends on the value the caller reads back.
 */
int puts(const char *);

static int slot;

/* Mirrors `void free(void*)`: void return, one pointer argument. */
static void sink(void *p) { *(int *)p = 1; }

int main(void) {
	slot = 0;
	sink(&slot);   /* void call with an argument: the bug-#1 repro */
	if (slot == 1)
		puts("voidcall: PASS");
	else
		puts("voidcall: FAIL");
	return 0;
}
