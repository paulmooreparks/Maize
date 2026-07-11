#include "all.h"

/* Maize C ABI lowering (maize-11 "Maize C ABI", decision 6416; varargs added in
 * maize-98).
 *
 * Scalar integer/pointer arguments pass in R0..R5 left to right, scalar return in
 * RV. R0..R5 are the six caller-saved GP argument registers; R6..R9 stay
 * callee-saved (maize-11 decision 6416), so the register allocator only ever
 * places call arguments in caller-saved registers (Option C, decision 7598).
 * Arguments past R5 (arg #7 onward) pass on the stack (the overflow convention,
 * decision 7538): the caller pushes them right-to-left so arg #7 sits at the
 * lowest address, landing at [BP+16], [BP+24], ... in the callee after its PUSH BP
 * (BP+0 = saved BP, BP+8 = return address); 8-byte slots, caller reserves and
 * releases.
 *
 * Variadic functions (decisions 7536-7540, re-tuned by 7598): a variadic callee
 * spills all six GP argument registers R0..R5 into a 48-byte register save area at
 * a constant BP-48..BP-0 (emit.c, guarded by fn->vararg), and va_list is the
 * 24-byte SysV gp-subset {gp_offset:u32 @0, reserved:u32 @4, overflow_arg_area:ptr
 * @8, reg_save_area:ptr @16}. selvastart / selvaarg are a near-verbatim port of
 * amd64/sysv.c: six GP arg registers matches x86-64 SysV, so the GP save-area
 * limit is 48 (amd64's own value) with no widening, and the floating-point branch
 * is dropped (Maize has no FP register file).
 *
 * Aggregate (struct-by-value) and environment/closure calls are still unsupported
 * and err() here rather than miscompiling silently. Floating-point arguments and
 * returns err() too (no FP register file).
 *
 * Layout of a call's RCall argument (our own, read back by maize_argregs /
 * maize_retregs):
 *
 *   bits 0..4 : number of GP args passed in R0..R5   (0..6)
 *   bit  5    : function returns a GP value in RV     (0..1)
 */

enum {
	RcNgpMask = 0x1f,
	RcRetGp   = 1 << 5,
};

struct Params {
	int ngp;    /* named GP arguments passed in registers (0..6) */
	int nstk;   /* named arguments passed on the stack (count of 8-byte slots) */
};

static int gpreg[6] = {R0, R1, R2, R3, R4, R5};

bits
maize_argregs(Ref r, int p[2])
{
	bits b;
	int ngp, i;

	assert(rtype(r) == RCall);
	ngp = r.val & RcNgpMask;
	if (p) {
		p[0] = ngp;
		p[1] = 0;
	}
	b = 0;
	for (i = 0; i < ngp; i++)
		b |= BIT(gpreg[i]);
	return b;
}

bits
maize_retregs(Ref r, int p[2])
{
	int rgp;

	assert(rtype(r) == RCall);
	rgp = (r.val & RcRetGp) != 0;
	if (p) {
		p[0] = rgp;
		p[1] = 0;
	}
	return rgp ? BIT(RV) : 0;
}

/* Per-argument class markers stored in preg[] (indexed by instruction position,
 * i - i0). A non-negative value is the assigned GP register; the two negatives
 * distinguish stack-passed real arguments from the variadic marker so the caller
 * loops process each correctly. */
enum {
	AcStk  = -1,   /* stack-passed argument (overflow past R9)      */
	AcSkip = -2,   /* the Oargv '...' marker: not an argument at all */
};

/* Classify a call's / function's arguments. Each of the first six GP arguments
 * takes a register (preg[k] = Rn); arguments past R5 are stack-passed (AcStk),
 * counted in *pnstk. The Oargv marker (variadic '...') is AcSkip: it occupies a
 * position in the i0..i1 range but is not an argument, so it must be skipped by
 * every per-argument loop. Variadic arguments after the marker pass exactly like
 * named ones. Returns the number of register GP arguments (0..6). */
static int
argsclass(Ins *i0, Ins *i1, int *preg, int *pnstk)
{
	int ngp, nstk;
	Ins *i;

	ngp = 0;
	nstk = 0;
	for (i = i0; i < i1; i++)
		switch (i->op) {
		case Opar:
		case Oarg:
			if (KBASE(i->cls) != 0)
				err("maize abi: floating-point arguments are not supported");
			if (ngp < 6)
				preg[i - i0] = gpreg[ngp++];
			else {
				preg[i - i0] = AcStk;
				nstk++;
			}
			break;
		case Oparc:
		case Oargc:
			err("maize abi: aggregate (struct) arguments are not supported");
		case Opare:
		case Oarge:
			err("maize abi: environment calls are not supported");
		case Oargv:
			preg[i - i0] = AcSkip;
			break;
		default:
			die("unreachable");
		}
	if (pnstk)
		*pnstk = nstk;
	return ngp;
}

static void
selret(Blk *b, Fn *fn)
{
	int j, k, cty;

	j = b->jmp.type;
	if (!isret(j) || j == Jret0)
		return;

	if (j == Jretc)
		err("maize abi: aggregate (struct) return is not supported");

	k = j - Jretw;
	if (KBASE(k) != 0)
		err("maize abi: floating-point return is not supported");

	emit(Ocopy, k, TMP(RV), b->jmp.arg, R);
	cty = RcRetGp;

	b->jmp.type = Jret0;
	b->jmp.arg = CALL(cty);
	(void)fn;
}

static void
selcall(Fn *fn, Ins *i0, Ins *i1)
{
	Ins *i;
	int reg[32], ngp, nstk, cty, n;
	uint off;
	Ref r, rstk;

	if (i1 - i0 > 32)
		err("maize abi: too many arguments");
	if (!req(i1->arg[1], R))
		err("maize abi: aggregate (struct) return is not supported");

	ngp = argsclass(i0, i1, reg, &nstk);
	cty = ngp & RcNgpMask;

	/* Release the caller-reserved overflow area after the call (emitted first,
	 * so it lands last in program order). */
	rstk = getcon(nstk * 8, fn);
	if (nstk)
		emit(Oadd, Kl, TMP(RS), TMP(RS), rstk);

	/* Return value: copy RV into the call's destination temporary. */
	if (KBASE(i1->cls) != 0)
		err("maize abi: floating-point return is not supported");
	/* Always emit the RV result-copy and mark a GP return, mirroring amd64
	 * sysv selcall. For a void call i1->to == R, producing a dead `copy R, RV`
	 * that (1) keeps spill.c's dopm engaged so T.argregs()/T.retregs() are
	 * applied to the call (without it the arg-register defs look dead ->
	 * spill.c:431 assert "dead reg"), and (2) is consumed by rega's
	 * parallel-move machinery, so it never reaches emit. */
	emit(Ocopy, i1->cls, i1->to, TMP(RV), R);
	cty |= RcRetGp;

	emit(Ocall, 0, R, i1->arg[0], CALL(cty));

	/* Move each register argument into its assigned register (emitted before
	 * the call in final program order because emit() builds the block
	 * backwards). */
	for (i = i0; i < i1; i++) {
		n = i - i0;
		if (reg[n] >= 0)
			emit(Ocopy, i->cls, TMP(reg[n]), i->arg[0], R);
	}

	/* Overflow arguments past R5: store each at SP + 8*k, the first overflow
	 * argument (arg #7) at SP+0 so it reaches the callee at [BP+16]. Whole
	 * 8-byte slots; a sub-int value occupies the low bytes, upper bytes are
	 * don't-care (little-endian, decision 7539). */
	off = 0;
	for (i = i0; i < i1; i++) {
		n = i - i0;
		if (reg[n] == AcStk) {
			r = newtmp("abi", Kl, fn);
			emit(Ostorel, 0, R, i->arg[0], r);
			emit(Oadd, Kl, r, TMP(RS), getcon(off, fn));
			off += 8;
		}
	}

	/* Reserve the overflow area before the argument stores (emitted last, so
	 * it lands first in program order). */
	if (nstk)
		emit(Osub, Kl, TMP(RS), TMP(RS), rstk);

	(void)fn;
}

static struct Params
selpar(Fn *fn, Ins *i0, Ins *i1)
{
	int reg[32], ngp, nstk, n, si;
	Ins *i;
	Ref r;

	if (i1 - i0 > 32)
		err("maize abi: too many parameters");

	curi = &insb[NIns];
	ngp = argsclass(i0, i1, reg, &nstk);
	fn->reg = maize_argregs(CALL(ngp & RcNgpMask), 0);

	si = 0;
	for (i = i0; i < i1; i++) {
		n = i - i0;
		if (reg[n] >= 0) {
			emit(Ocopy, i->cls, i->to, TMP(reg[n]), R);
		} else if (reg[n] == AcStk) {
			/* Named stack parameter at [BP + 16 + 8*si] (decision 7538). */
			r = newtmp("abi", Kl, fn);
			emit(Oload, i->cls, i->to, r, R);
			emit(Oadd, Kl, r, TMP(RB), getcon(16 + 8 * si, fn));
			si++;
		}
	}
	return (struct Params){.ngp = ngp, .nstk = nstk};
}

static Blk *
split(Fn *fn, Blk *b)
{
	Blk *bn;

	++fn->nblk;
	bn = blknew();
	bn->nins = &insb[NIns] - curi;
	idup(&bn->ins, curi, bn->nins);
	curi = &insb[NIns];
	bn->visit = ++b->visit;
	(void)!snprintf(bn->name, NString, "%s.%d", b->name, b->visit);
	bn->loop = b->loop;
	bn->link = b->link;
	b->link = bn;
	return bn;
}

static void
chpred(Blk *b, Blk *bp, Blk *bp1)
{
	Phi *p;
	uint a;

	for (p = b->phi; p; p = p->link) {
		for (a = 0; p->blk[a] != bp; a++)
			assert(a + 1 < p->narg);
		p->blk[a] = bp1;
	}
}

/* Port of amd64/sysv.c:selvaarg, GP path only (Maize has no FP register file).
 * va_arg reads the next argument from the register save area while gp_offset < 48
 * (all six GP slots), else from the overflow area, bumping the cursor by 8 either
 * way (every slot is 8-byte-strided, decision 7539).
 *
 *   @b [...]
 *       r0 =l add ap, 0
 *       nr =l loadsw r0            ; gp_offset
 *       r1 =w cultw nr, 48
 *       jnz r1, @breg, @bstk
 *   @breg
 *       r0 =l add ap, 16
 *       r1 =l loadl r0            ; reg_save_area
 *       lreg =l add r1, nr
 *       r0 =w add nr, 8
 *       r1 =l add ap, 0
 *       storew r0, r1             ; gp_offset += 8
 *   @bstk
 *       r0 =l add ap, 8
 *       lstk =l loadl r0          ; overflow_arg_area
 *       r1 =l add lstk, 8
 *       storel r1, r0             ; overflow_arg_area += 8
 *   @b0
 *       %loc =l phi @breg %lreg, @bstk %lstk
 *       i->to =(i->cls) load %loc
 */
static void
selvaarg(Fn *fn, Blk *b, Ins *i)
{
	Ref loc, lreg, lstk, nr, r0, r1, c8, c16, c48, ap;
	Blk *b0, *bstk, *breg;

	if (KBASE(i->cls) != 0)
		err("maize abi: floating-point va_arg is not supported");

	c8 = getcon(8, fn);
	c16 = getcon(16, fn);
	c48 = getcon(48, fn);
	ap = i->arg[0];

	loc = newtmp("abi", Kl, fn);
	emit(Oload, i->cls, i->to, loc, R);
	b0 = split(fn, b);
	b0->jmp = b->jmp;
	b0->s1 = b->s1;
	b0->s2 = b->s2;
	if (b->s1)
		chpred(b->s1, b, b0);
	if (b->s2 && b->s2 != b->s1)
		chpred(b->s2, b, b0);

	lreg = newtmp("abi", Kl, fn);
	nr = newtmp("abi", Kl, fn);
	r0 = newtmp("abi", Kw, fn);
	r1 = newtmp("abi", Kl, fn);
	emit(Ostorew, Kw, R, r0, r1);
	emit(Oadd, Kl, r1, ap, CON_Z);
	emit(Oadd, Kw, r0, nr, c8);
	r0 = newtmp("abi", Kl, fn);
	r1 = newtmp("abi", Kl, fn);
	emit(Oadd, Kl, lreg, r1, nr);
	emit(Oload, Kl, r1, r0, R);
	emit(Oadd, Kl, r0, ap, c16);
	breg = split(fn, b);
	breg->jmp.type = Jjmp;
	breg->s1 = b0;

	lstk = newtmp("abi", Kl, fn);
	r0 = newtmp("abi", Kl, fn);
	r1 = newtmp("abi", Kl, fn);
	emit(Ostorel, Kw, R, r1, r0);
	emit(Oadd, Kl, r1, lstk, c8);
	emit(Oload, Kl, lstk, r0, R);
	emit(Oadd, Kl, r0, ap, c8);
	bstk = split(fn, b);
	bstk->jmp.type = Jjmp;
	bstk->s1 = b0;

	b0->phi = alloc(sizeof *b0->phi);
	*b0->phi = (Phi){
		.cls = Kl, .to = loc,
		.narg = 2,
		.blk = vnew(2, sizeof b0->phi->blk[0], Pfn),
		.arg = vnew(2, sizeof b0->phi->arg[0], Pfn),
	};
	b0->phi->blk[0] = bstk;
	b0->phi->blk[1] = breg;
	b0->phi->arg[0] = lstk;
	b0->phi->arg[1] = lreg;
	r0 = newtmp("abi", Kl, fn);
	r1 = newtmp("abi", Kw, fn);
	b->jmp.type = Jjnz;
	b->jmp.arg = r1;
	b->s1 = breg;
	b->s2 = bstk;
	emit(Ocmpw+Ciult, Kw, r1, nr, c48);
	emit(Oloadsw, Kl, nr, r0, R);
	emit(Oadd, Kl, r0, ap, CON_Z);
}

/* Port of amd64/sysv.c:selvastart, GP-only. Initializes the 24-byte va_list:
 *   gp_offset          = ngp*8            @ ap+0   (named GP args already consumed)
 *   _reserved          = 48               @ ap+4   (SysV fp_offset; inert on Maize)
 *   overflow_arg_area  = BP + 16 + nstk*8 @ ap+8   (first unnamed stack arg)
 *   reg_save_area      = BP - 48          @ ap+16  (base of the 48-byte save area) */
static void
selvastart(Fn *fn, struct Params p, Ref ap)
{
	Ref r0, r1;
	int gp;

	gp = p.ngp * 8;

	r0 = newtmp("abi", Kl, fn);
	r1 = newtmp("abi", Kl, fn);
	emit(Ostorel, Kw, R, r1, r0);
	emit(Oadd, Kl, r1, TMP(RB), getcon(-48, fn));
	emit(Oadd, Kl, r0, ap, getcon(16, fn));

	r0 = newtmp("abi", Kl, fn);
	r1 = newtmp("abi", Kl, fn);
	emit(Ostorel, Kw, R, r1, r0);
	emit(Oadd, Kl, r1, TMP(RB), getcon(16 + p.nstk * 8, fn));
	emit(Oadd, Kl, r0, ap, getcon(8, fn));

	r0 = newtmp("abi", Kl, fn);
	emit(Ostorew, Kw, R, getcon(48, fn), r0);
	emit(Oadd, Kl, r0, ap, getcon(4, fn));

	emit(Ostorew, Kw, R, getcon(gp, fn), ap);
}

void
maize_abi(Fn *fn)
{
	Blk *b;
	Ins *i, *i0, *ip;
	int n;
	struct Params p;

	for (b = fn->start; b; b = b->link)
		b->visit = 0;

	/* Lower parameters in the entry block. */
	for (b = fn->start, i = b->ins; i < &b->ins[b->nins]; i++)
		if (!ispar(i->op))
			break;
	p = selpar(fn, b->ins, i);
	n = b->nins - (i - b->ins) + (&insb[NIns] - curi);
	i0 = alloc(n * sizeof(Ins));
	ip = icpy(ip = i0, curi, &insb[NIns] - curi);
	ip = icpy(ip, i, &b->ins[b->nins] - i);
	b->nins = n;
	b->ins = i0;

	/* Lower calls, returns, and vararg instructions in every block. selvaarg
	 * splits blocks (appending to the b->link chain), so iterate with the
	 * visit-flag guard the amd64/arm64 ABI passes use: split blocks are marked
	 * visited and skipped, and fn->start is processed last. */
	b = fn->start;
	do {
		if (!(b = b->link))
			b = fn->start;
		if (b->visit)
			continue;
		curi = &insb[NIns];
		selret(b, fn);
		for (i = &b->ins[b->nins]; i != b->ins;)
			switch ((--i)->op) {
			default:
				emiti(*i);
				break;
			case Ocall:
				for (i0 = i; i0 > b->ins; i0--)
					if (!isarg((i0 - 1)->op))
						break;
				selcall(fn, i0, i);
				i = i0;
				break;
			case Ovastart:
				selvastart(fn, p, i->arg[0]);
				break;
			case Ovaarg:
				selvaarg(fn, b, i);
				break;
			case Oarg:
			case Oargc:
				die("unreachable");
			}
		b->nins = &insb[NIns] - curi;
		idup(&b->ins, curi, b->nins);
	} while (b != fn->start);

	if (debug['A']) {
		fprintf(stderr, "\n> After ABI lowering:\n");
		printfn(fn, stderr);
	}
}
