/* maize-136 spill regression: force QBE's register allocator to spill and drive
   every spilled-operand emit path in the Maize target (toolchain/qbe-maize/emit.c).

   The allocatable GP pool is 11 (R0..R9,RV; RT/RB/RS are reserved). Seventeen
   simultaneously-live opaque()-barriered values (v0..v15 + acc) push well past
   that ceiling, so QBE MUST spill several to frame slots. Before this card the
   emitter die()d on any spilled operand; this fixture is the end-to-end gate that
   it no longer does, and that the spilled data is computed correctly.

   All three spilled-operand forms are exercised (verified by inspecting the
   emitted .mazm at implementation time):
     - reload      : LD @RT after a LEA, from a spilled Oload,
     - spill store : ST @RT after a LEA, from a spilled Ostore,
     - block-edge slot->slot Ocopy : the riskiest new lowering, which needs a
       value register and an address register at once and so borrows one register
       via PUSH/POP. The loop shifts sixteen loop-carried values by one every
       iteration (v_{k} <- v_{k-1}; v0 <- a fresh, state-dependent value). This is
       a chain, not a register-only cycle, and because all sixteen values are
       simultaneously live (folded into acc each pass) they interfere and receive
       distinct frame slots, so the shift lowers to slot->slot copies. Each value
       feeds the final checked sum, so a miscompiled slot->slot copy changes the
       result and fails the exact-stdout gate rather than passing vacuously.

   Unsigned arithmetic makes every operation wrap deterministically (mod 2^32) on
   both Maize and the host, so the expected constant is exact. It was cross-checked
   against the identical computation built with the host compiler at -O0 and -O2.

   Self-checking, single line of stdout: "spill: PASS". */

int puts(const char *);

/* Non-inlined optimization barrier. cproc/qbe do no cross-function inlining, so a
   value returned through opaque() is opaque to qbe/fold.c and cannot be folded to
   a compile-time constant (without this the values collapse and never spill). */
static unsigned opaque(unsigned x) { return x; }

int main(void) {
	unsigned v0 = opaque(1),  v1 = opaque(2),  v2 = opaque(3),  v3 = opaque(4);
	unsigned v4 = opaque(5),  v5 = opaque(6),  v6 = opaque(7),  v7 = opaque(8);
	unsigned v8 = opaque(9),  v9 = opaque(10), v10 = opaque(11), v11 = opaque(12);
	unsigned v12 = opaque(13), v13 = opaque(14), v14 = opaque(15), v15 = opaque(16);
	unsigned acc = opaque(0);
	int r;

	for (r = 0; r < 25; r++) {
		/* Fold all sixteen values into acc so every one is live here; this is
		   what forces them to interfere and take distinct spill slots. */
		acc = acc + v0 - v1 + v2 - v3 + v4 - v5 + v6 - v7
			  + v8 - v9 + v10 - v11 + v12 - v13 + v14 - v15;
		/* Shift left by one with a fresh, state-dependent input at v0. A chain
		   (no wraparound cycle), so under spill the phi resolution at the loop
		   back-edge emits slot->slot copies rather than a register-only swap. */
		unsigned nv0 = v0 * 31u + acc + 7u;
		v15 = v14; v14 = v13; v13 = v12; v12 = v11; v11 = v10; v10 = v9; v9 = v8;
		v8 = v7; v7 = v6; v6 = v5; v5 = v4; v4 = v3; v3 = v2; v2 = v1; v1 = v0;
		v0 = nv0;
	}

	unsigned sum = v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7
		     + v8 + v9 + v10 + v11 + v12 + v13 + v14 + v15 + acc;

	puts(sum == 1541762618u ? "spill: PASS" : "spill: FAIL");
	return 0;
}
