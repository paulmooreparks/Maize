#include "all.h"

/* Maize instruction selection (maize-63, full single-TU coverage).
 *
 * Maize is CISC: the ALU keeps memory and immediate operands, so QBE constants
 * flow straight through to emit as immediates and no separate "load immediate"
 * pass is needed. isel does three things here:
 *
 *   1. Rewrites stack-slot temps into explicit frame-relative address
 *      materializations (Oaddr), via fixarg.
 *   2. Canonicalizes the comparison family so the constant operand (if any) sits
 *      in the CMP src position: `CMP src dst` computes `dst - src`, and QBE
 *      evaluates `cmp<cc> arg0,arg1` as `arg0 <cc> arg1`, so the natural lowering
 *      is `CMP arg1 arg0`; when arg0 is the constant we swap operands and swap the
 *      condition (cmpop) so the register lands in dst. emit does the CMP+SETcc /
 *      CMP+Jcc.  (decisions 6775, 6776)
 *   3. Lowers conditional branches (seljmp): compare/branch fusion when the
 *      comparison feeds only the jnz, else an explicit `CMP $00 <reg>` on the
 *      branched value since data movement does not set flags (decision 6779).
 *
 * The 3-address -> 2-address reconciliation for the ALU ops is done in emit
 * (where physical-register aliasing is observable), so sel leaves those ops in
 * their QBE 3-address form. Phi resolution is inherited from QBE's target-
 * independent register allocator (rega.c); the backend adds no phi handling.
 */

static void
fixarg(Ref *pr, Fn *fn)
{
	Ref r0, r1;
	int s;

	r0 = *pr;
	if (rtype(r0) == RTmp) {
		s = fn->tmp[r0.val].slot;
		if (s != -1) {
			r1 = newtmp("isel", Kl, fn);
			emit(Oaddr, Kl, r1, SLOT(s), R);
			*pr = r1;
		}
	}
}

/* The op carrying integer condition c (c in [0,NCmpI)) for class k (Kw or Kl). */
static int
cmpop_for(int k, int c)
{
	return (k == Kw ? Ocmpw : Ocmpl) + c;
}

/* The op carrying float condition c (c in [NCmpI,NCmp)) for class k (Ks or Kd). */
static int
fcmpop_for(int k, int c)
{
	return (k == Ks ? Ocmps : Ocmpd) + (c - NCmpI);
}

/* Operand-swap canonicalization for a float compare, mirroring amd64/isel.c
 * cmpswap: `lt`/`le` always swap to `gt`/`ge`; the symmetric relations
 * (eq/ne/o/uo) swap only when arg0 is a constant (so the FCMP dst can be a
 * register); `gt`/`ge` never swap. On a swap the caller applies cmpop(c). */
static int
fcmpswap(Ref arg[2], int c)
{
	switch (c) {
	case NCmpI+Cflt:
	case NCmpI+Cfle:
		return 1;
	case NCmpI+Cfgt:
	case NCmpI+Cfge:
		return 0;
	}
	return rtype(arg[0]) == RCon;
}

/* Lower a float comparison (maize-137). Canonicalize operands exactly like
 * amd64/isel.c selcmp: swap per fcmpswap and swap the condition via cmpop, then
 * rebuild the compare op for the surviving relation. The FCMP dst (arg0 == `a`)
 * MUST be a register, so a surviving const dst is materialized into a fresh temp
 * with the reclassed GP class (Ks->Kw, Kd->Kl) since the isel reclass pre-pass
 * has already run. emit reads the (untouched) Ins.cls Ks/Kd to pick the FCMP
 * mnemonic and the SETcc predicate; `to` decides value (SETcc) vs flag-only
 * (a fused block Jcc consumes the flags). */
static void
selfcmp(Ins i, int kc, int c, Fn *fn)
{
	Ref *a, cst, t;
	int swap, gpk;

	swap = fcmpswap(i.arg, c);
	if (swap) {
		t = i.arg[0];
		i.arg[0] = i.arg[1];
		i.arg[1] = t;
		c = cmpop(c);
	}
	i.op = fcmpop_for(kc, c);
	emiti(i);
	a = curi->arg;
	if (rtype(a[0]) == RCon) {
		cst = a[0];
		gpk = KWIDE(kc) ? Kl : Kw;
		t = newtmp("isel", gpk, fn);
		a[0] = t;
		emit(Ocopy, gpk, t, cst, R);
	}
	fixarg(&a[0], fn);
	fixarg(&a[1], fn);
}

static void
sel(Ins i, Fn *fn)
{
	Ref *a;
	int kc, c;

	if (i.op == Onop)
		return;
	if (i.op == Ocall) {
		/* Keep the call target as-is (a CAddr constant becomes a direct
		 * `CALL label`; a register becomes an indirect `CALL Rn`). */
		emiti(i);
		return;
	}
	if (i.op == Ocast)
		/* Zfinx int<->float bitcast of equal width is a bit-identical
		 * register move: lower to a plain copy (emitcopy handles it). */
		i.op = Ocopy;

	if (iscmp(i.op, &kc, &c)) {
		if (c >= NCmpI) {
			selfcmp(i, kc, c, fn);
			return;
		}
		emiti(i);
		a = curi->arg;
		if (rtype(a[0]) == RCon) {
			/* The constant must occupy the CMP src (arg1) slot; swap
			 * operands and swap the condition so the register is dst. */
			Ref t = a[0];
			a[0] = a[1];
			a[1] = t;
			curi->op = cmpop_for(kc, cmpop(c));
		}
		fixarg(&a[0], fn);
		fixarg(&a[1], fn);
		return;
	}

	switch (i.op) {
	case Ocopy:
	case Oadd:
	case Osub:
	case Omul:
	case Oand:
	case Oor:
	case Oxor:
	case Oshl:
	case Oshr:
	case Osar:
	case Odiv:
	case Orem:
	case Oudiv:
	case Ourem:
	case Ostoreb:
	case Ostoreh:
	case Ostorew:
	case Ostorel:
	case Ostores:
	case Ostored:
	case Oload:
	case Oloadsb:
	case Oloadub:
	case Oloadsh:
	case Oloaduh:
	case Oloadsw:
	case Oloaduw:
	case Oextsb:
	case Oextub:
	case Oextsh:
	case Oextuh:
	case Oextsw:
	case Oextuw:
	case Oexts:
	case Otruncd:
	case Ostosi:
	case Odtosi:
	case Oswtof:
	case Osltof:
		emiti(i);
		a = curi->arg;
		fixarg(&a[0], fn);
		fixarg(&a[1], fn);
		return;
	default:
		err("maize isel: unsupported op '%s' (maize-63)", optab[i.op].name);
	}
}

/* If the block's last instruction is a comparison (integer or float), return
 * it. seljmp decides whether it is a fusable branch predicate. */
static Ins *
lastcmp(Blk *b)
{
	Ins *i;
	int kc, c;

	if (b->nins == 0)
		return 0;
	i = &b->ins[b->nins - 1];
	if (iscmp(i->op, &kc, &c))
		return i;
	return 0;
}

static void
seljmp(Blk *b, Fn *fn)
{
	Ref r;
	Ins *fi;
	Tmp *t;
	int kc, c, swap, fused;

	switch (b->jmp.type) {
	case Jret0:
	case Jjmp:
	case Jhlt:
		return;
	case Jjnz:
		break;
	default:
		err("maize isel: unsupported control flow");
	}

	r = b->jmp.arg;
	assert(rtype(r) == RTmp);
	b->jmp.arg = R;
	t = &fn->tmp[r.val];

	if (b->s1 == b->s2) {
		b->jmp.type = Jjmp;
		b->s2 = 0;
		return;
	}

	fused = 0;
	fi = lastcmp(b);
	if (fi && req(fi->to, r) && t->nuse == 1) {
		iscmp(fi->op, &kc, &c);
		if (c < NCmpI) {
			/* Integer compare/branch fusion: reuse the comparison as a
			 * flag-only CMP (to = R => emit emits CMP without a SETcc)
			 * and branch on it. */
			if (rtype(fi->arg[0]) == RCon) {
				Ref tmp = fi->arg[0];
				fi->arg[0] = fi->arg[1];
				fi->arg[1] = tmp;
				c = cmpop(c);
				fi->op = cmpop_for(kc, c);
			}
			fi->to = R;
			b->jmp.type = Jjf + c;
			fused = 1;
		} else {
			/* Float compare/branch fusion: only {gt, ge, uo} map to a
			 * single Maize Jcc (JA / JAE / JP). lt/le canonicalize to
			 * gt/ge via the same swap sel()/selfcmp applies, so they
			 * fuse too; eq/ne/o need a multi-instruction predicate and
			 * are left as a 0/1 value for the CMP $00 fallback below.
			 * Do not mutate fi here: sel()/selfcmp performs the swap,
			 * op rewrite, and any const-dst materialization when it
			 * emits the (now flag-only) FCMP, and its canonicalization
			 * is deterministic, so the surviving relation equals the
			 * one computed here and the jump type stays in sync. */
			swap = fcmpswap(fi->arg, c);
			if (swap)
				c = cmpop(c);
			if (c == NCmpI+Cfgt || c == NCmpI+Cfge
			|| c == NCmpI+Cfuo) {
				fi->to = R;
				b->jmp.type = Jjf + c;
				fused = 1;
			}
		}
	}
	if (!fused) {
		/* Branch on a value (a non-comparison, or a non-fusable float
		 * compare materialized as 0/1): data movement does not set flags,
		 * so materialize an explicit `CMP $00 <reg>` (arg1 == 0, arg0 ==
		 * r => CMP arg1 arg0) and take the nonzero edge with JNZ. */
		kc = fn->tmp[r.val].cls;
		if (kc != Kw && kc != Kl)
			err("maize isel: unsupported branch value class");
		emit(cmpop_for(kc, Cine), kc, R, r, CON_Z);
		fixarg(&curi->arg[0], fn);
		b->jmp.type = Jjf + Cine;
	}
}

void
maize_isel(Fn *fn)
{
	Blk *b, **sb;
	Ins *i;
	Phi *p;
	uint n, al;
	int64_t sz;

	/* Float register-class reclass pre-pass (maize-137). Under Zfinx there is
	 * one physical register file, so float values are allocated into the GP
	 * registers alongside ints. QBE's target-independent allocator and spiller
	 * key on tmp[t].cls (never Ins.cls: rega.c bank select / parallel-move,
	 * spill.c pressure), so rewriting every float temp's class Ks->Kw, Kd->Kl
	 * places floats in R0..R9/RV exactly like ints, with zero qbe-core change.
	 * KWIDE is preserved by the mapping (Ks/Kw both 32-bit H0, Kd/Kl both
	 * 64-bit W0), so spill-slot and register-move widths stay correct. The
	 * float-ness rides ONLY on each operation's untouched Ins.cls (Ks/Kd),
	 * which emit reads to pick the FP mnemonic; nothing downstream reads a
	 * temp's cls expecting Ks/Kd after this. Runs before the alloc-slot pass
	 * and the block loop; isel runs before spill/rega. */
	for (n = 0; n < (uint)fn->ntmp; n++) {
		if (fn->tmp[n].cls == Ks)
			fn->tmp[n].cls = Kw;
		else if (fn->tmp[n].cls == Kd)
			fn->tmp[n].cls = Kl;
	}

	/* Assign frame slots to stack allocations (Oalloc). */
	b = fn->start;
	for (al = Oalloc, n = 4; al <= Oalloc1; al++, n *= 2)
		for (i = b->ins; i < &b->ins[b->nins]; i++)
			if (i->op == al) {
				if (rtype(i->arg[0]) != RCon)
					err("maize isel: dynamic alloc is not supported");
				sz = fn->con[i->arg[0].val].bits.i;
				if (sz < 0 || sz >= INT_MAX - 15)
					err("maize isel: invalid alloc size %"PRId64, sz);
				sz = (sz + n - 1) & -n;
				sz /= 4;
				fn->tmp[i->to.val].slot = fn->slot;
				fn->slot += sz;
				*i = (Ins){.op = Onop};
			}

	for (b = fn->start; b; b = b->link) {
		curi = &insb[NIns];
		/* Materialize slot-address temps that reach a successor phi
		 * argument (an `&local` used directly as a loop-carried phi
		 * value). isel emits in reverse into curi, so running this pass
		 * first lands the Oaddr (LEA) at the END of the predecessor
		 * block, after any fused flag-only CMP the block Jcc consumes;
		 * safe because maize LEA is flag-neutral (src/cpu.cpp; maize-4).
		 * Without it the alloc temp survives to rega and is resolved on
		 * the phi edge as a plain slot MOVE (the local's contents), not
		 * a LEA (its address): a silent miscompile. amd64/isel.c and
		 * arm64/isel.c run the identical pass (maize-103). */
		for (sb = (Blk*[3]){b->s1, b->s2, 0}; *sb; sb++)
			for (p = (*sb)->phi; p; p = p->link) {
				for (n = 0; p->blk[n] != b; n++)
					assert(n + 1 < p->narg);
				fixarg(&p->arg[n], fn);
			}
		seljmp(b, fn);
		for (i = &b->ins[b->nins]; i != b->ins;)
			sel(*--i, fn);
		b->nins = &insb[NIns] - curi;
		idup(&b->ins, curi, b->nins);
	}

	if (debug['I']) {
		fprintf(stderr, "\n> After instruction selection:\n");
		printfn(fn, stderr);
	}
}
