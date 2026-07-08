#include "all.h"

/* Maize instruction selection (maize-62, hello-world slice).
 *
 * Maize is CISC: the ALU keeps memory and immediate operands, so QBE constants
 * flow straight through to emit as immediates and no separate "load immediate"
 * pass is needed. The only rewriting isel does here is turning stack-slot temps
 * into explicit frame-relative address materializations (Oaddr). The full
 * instruction-selection matrix (comparisons materialized as 0/1, sign/zero
 * extending loads, div/mod, shifts, ...) is maize-63's job and is not reached by
 * hello world; unreached shapes err() rather than miscompile.
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

static void
sel(Ins i, Fn *fn)
{
	Ref *iarg;

	switch (i.op) {
	case Onop:
		return;
	case Ocall:
		/* Keep the call target as-is (a CAddr constant becomes a direct
		 * `CALL label`; a register becomes an indirect `CALL Rn`). */
		emiti(i);
		return;
	case Ocopy:
	case Oadd:
	case Osub:
	case Omul:
	case Oand:
	case Oor:
	case Oxor:
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
		emiti(i);
		iarg = curi->arg;
		fixarg(&iarg[0], fn);
		fixarg(&iarg[1], fn);
		return;
	default:
		err("maize isel: unsupported op '%s' (maize-63)", optab[i.op].name);
	}
}

static void
seljmp(Blk *b, Fn *fn)
{
	(void)fn;
	switch (b->jmp.type) {
	case Jret0:
	case Jjmp:
		return;
	default:
		err("maize isel: unsupported control flow (conditional branch is maize-63)");
	}
}

void
maize_isel(Fn *fn)
{
	Blk *b;
	Ins *i;
	uint n, al;
	int64_t sz;

	/* Assign frame slots to stack allocations (Oalloc). Not reached by hello,
	 * but kept so a stray fixed-size alloc lowers rather than crashes. */
	b = fn->start;
	for (al = Oalloc, n = 4; al <= Oalloc1; al++, n *= 2)
		for (i = b->ins; i < &b->ins[b->nins]; i++)
			if (i->op == al) {
				if (rtype(i->arg[0]) != RCon)
					err("maize isel: dynamic alloc is not supported (maize-63)");
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
