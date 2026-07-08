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

/* The op carrying condition c for class k (Kw or Kl). */
static int
cmpop_for(int k, int c)
{
	return (k == Kw ? Ocmpw : Ocmpl) + c;
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

	if (iscmp(i.op, &kc, &c)) {
		if (c >= NCmpI)
			err("maize isel: floating-point comparison is not supported");
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
		emiti(i);
		a = curi->arg;
		fixarg(&a[0], fn);
		fixarg(&a[1], fn);
		return;
	default:
		err("maize isel: unsupported op '%s' (maize-63)", optab[i.op].name);
	}
}

/* If the block's last instruction is an integer comparison, return it. */
static Ins *
lastcmp(Blk *b)
{
	Ins *i;
	int kc, c;

	if (b->nins == 0)
		return 0;
	i = &b->ins[b->nins - 1];
	if (iscmp(i->op, &kc, &c) && c < NCmpI)
		return i;
	return 0;
}

static void
seljmp(Blk *b, Fn *fn)
{
	Ref r;
	Ins *fi;
	Tmp *t;
	int kc, c;

	switch (b->jmp.type) {
	case Jret0:
	case Jjmp:
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

	fi = lastcmp(b);
	if (fi && req(fi->to, r) && t->nuse == 1) {
		/* Compare/branch fusion: reuse the comparison as a flag-only CMP
		 * (to = R => emit emits CMP without a SETcc) and branch on it. */
		iscmp(fi->op, &kc, &c);
		if (rtype(fi->arg[0]) == RCon) {
			Ref tmp = fi->arg[0];
			fi->arg[0] = fi->arg[1];
			fi->arg[1] = tmp;
			c = cmpop(c);
			fi->op = cmpop_for(kc, c);
		}
		fi->to = R;
		b->jmp.type = Jjf + c;
	} else {
		/* Branch on a non-comparison value: data movement does not set
		 * flags, so materialize an explicit `CMP $00 <reg>` (arg1 == 0,
		 * arg0 == r => CMP arg1 arg0) and take the nonzero edge with JNZ. */
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
	Blk *b;
	Ins *i;
	uint n, al;
	int64_t sz;

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
