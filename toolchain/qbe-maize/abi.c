#include "all.h"

/* Maize C ABI lowering (maize-11 "Maize C ABI", decision 6416).
 *
 * Scope this card (maize-62, hello world): scalar integer/pointer arguments in
 * R0..R9, scalar return in RV, direct calls, and constant/scalar returns. The
 * aggregate (struct-by-value), environment, and vararg paths, plus stack-passed
 * argument overflow past R9, are deferred to maize-63 and err() here rather than
 * miscompiling silently.
 *
 * Layout of a call's RCall argument (our own, read back by maize_argregs /
 * maize_retregs):
 *
 *   bits 0..4 : number of GP args passed in R0..R9   (0..10)
 *   bit  5    : function returns a GP value in RV     (0..1)
 */

enum {
	RcNgpMask = 0x1f,
	RcRetGp   = 1 << 5,
};

static int gpreg[10] = {R0, R1, R2, R3, R4, R5, R6, R7, R8, R9};

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

static int
argsclass(Ins *i0, Ins *i1, int *preg)
{
	int ngp;
	Ins *i;

	ngp = 0;
	for (i = i0; i < i1; i++)
		switch (i->op) {
		case Opar:
		case Oarg:
			if (KBASE(i->cls) != 0)
				err("maize abi: floating-point arguments are not supported (maize-63)");
			if (ngp >= 10)
				err("maize abi: more than 10 register arguments / stack overflow not supported (maize-63)");
			preg[i - i0] = gpreg[ngp++];
			break;
		case Oparc:
		case Oargc:
			err("maize abi: aggregate (struct) arguments are not supported (maize-63)");
		case Opare:
		case Oarge:
			err("maize abi: environment calls are not supported (maize-63)");
		case Oargv:
			err("maize abi: varargs are not supported (maize-63)");
		default:
			die("unreachable");
		}
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
		err("maize abi: aggregate (struct) return is not supported (maize-63)");

	k = j - Jretw;
	if (KBASE(k) != 0)
		err("maize abi: floating-point return is not supported (maize-63)");

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
	int reg[32], ngp, cty;

	if (i1 - i0 > 32)
		err("maize abi: too many arguments (maize-63)");
	if (!req(i1->arg[1], R))
		err("maize abi: aggregate (struct) return is not supported (maize-63)");

	ngp = argsclass(i0, i1, reg);
	cty = ngp & RcNgpMask;

	/* Return value: copy RV into the call's destination temporary. */
	if (KBASE(i1->cls) != 0)
		err("maize abi: floating-point return is not supported (maize-63)");
	if (!req(i1->to, R)) {
		emit(Ocopy, i1->cls, i1->to, TMP(RV), R);
		cty |= RcRetGp;
	}

	emit(Ocall, 0, R, i1->arg[0], CALL(cty));

	/* Move each argument into its assigned register (emitted before the
	 * call in final program order because emit() builds the block backwards). */
	for (i = i0; i < i1; i++)
		emit(Ocopy, i->cls, TMP(reg[i - i0]), i->arg[0], R);

	(void)fn;
}

static int
selpar(Fn *fn, Ins *i0, Ins *i1)
{
	int reg[32], ngp, n;
	Ins *i;

	if (i1 - i0 > 32)
		err("maize abi: too many parameters (maize-63)");

	curi = &insb[NIns];
	ngp = argsclass(i0, i1, reg);
	fn->reg = maize_argregs(CALL(ngp & RcNgpMask), 0);

	for (i = i0; i < i1; i++) {
		n = i - i0;
		emit(Ocopy, i->cls, i->to, TMP(reg[n]), R);
	}
	return ngp;
}

void
maize_abi(Fn *fn)
{
	Blk *b;
	Ins *i, *i0, *ip;
	int n;

	/* Lower parameters in the entry block. */
	for (b = fn->start, i = b->ins; i < &b->ins[b->nins]; i++)
		if (!ispar(i->op))
			break;
	selpar(fn, b->ins, i);
	n = b->nins - (i - b->ins) + (&insb[NIns] - curi);
	i0 = alloc(n * sizeof(Ins));
	ip = icpy(ip = i0, curi, &insb[NIns] - curi);
	ip = icpy(ip, i, &b->ins[b->nins] - i);
	b->nins = n;
	b->ins = i0;

	/* Lower calls and returns in every block. */
	for (b = fn->start; b; b = b->link) {
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
			case Oarg:
			case Oargc:
				die("unreachable");
			}
		b->nins = &insb[NIns] - curi;
		idup(&b->ins, curi, b->nins);
	}

	if (debug['A']) {
		fprintf(stderr, "\n> After ABI lowering:\n");
		printfn(fn, stderr);
	}
}
