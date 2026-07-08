#include "all.h"

/* Maize assembly emission (maize-62, hello-world slice).
 *
 * Emits mazm mnemonics (CP, CALL, PUSH, POP, SUB, RET, ...), never raw opcode
 * bytes; mazm assembles them to the maize-64 encoding. The emitted surface is
 * exactly what hello world reaches (maize-11 "Back-end instruction-selection
 * coverage", hello subset): global address materialization, argument-in-R0,
 * CALL, a constant return into RV, and the prologue/epilogue. Everything else
 * die()s so maize-63 adds it deliberately rather than inheriting silent bugs.
 */

typedef struct E E;
struct E {
	FILE *f;
	Fn *fn;
	uint64_t frame;   /* bytes reserved by the prologue SUB */
	uint nsaved;      /* callee-saved registers preserved */
};

static char *
rname(int r)
{
	switch (r) {
	case R0: return "R0";
	case R1: return "R1";
	case R2: return "R2";
	case R3: return "R3";
	case R4: return "R4";
	case R5: return "R5";
	case R6: return "R6";
	case R7: return "R7";
	case R8: return "R8";
	case R9: return "R9";
	case RV: return "RV";
	case RT: return "RT";
	case RB: return "BP";
	case RS: return "SP";
	default: die("maize emit: invalid register %d", r);
	}
	return 0;
}

/* Emit a constant as a mazm immediate operand (no destination). Integer
 * constants are printed at their class width so the sign-extending CP that
 * consumes them reproduces the exact value; address constants become a full-64
 * label reference (maize-11 decision 6415), materialized into the whole
 * destination register by the consuming CP. */
static void
emitcon(Con *c, int k, E *e)
{
	int w;

	switch (c->type) {
	case CBits:
		w = KWIDE(k) ? 16 : 8;
		fprintf(e->f, "$%0*"PRIx64, w, (uint64_t)c->bits.i);
		break;
	case CAddr:
		if (c->bits.i != 0)
			die("maize emit: nonzero address offset is not supported (maize-63)");
		fprintf(e->f, "%s", maize_sym(str(c->label)));
		break;
	default:
		die("maize emit: undefined constant");
	}
}

static void
emitcopy(Ins *i, E *e)
{
	Ref r;
	Con *c;

	if (req(i->to, i->arg[0]))
		return;
	assert(isreg(i->to));
	r = i->arg[0];
	switch (rtype(r)) {
	case RCon:
		c = &e->fn->con[r.val];
		fputs("\tCP\t", e->f);
		emitcon(c, i->cls, e);
		fprintf(e->f, " %s\n", rname(i->to.val));
		break;
	case RTmp:
		assert(isreg(r));
		fprintf(e->f, "\tCP\t%s %s\n", rname(r.val), rname(i->to.val));
		break;
	default:
		die("maize emit: unsupported copy source");
	}
}

static void
emitcall(Ins *i, E *e)
{
	Ref r;
	Con *c;

	r = i->arg[0];
	switch (rtype(r)) {
	case RCon:
		c = &e->fn->con[r.val];
		if (c->type != CAddr || c->bits.i != 0)
			die("maize emit: unsupported call target");
		fprintf(e->f, "\tCALL\t%s\n", maize_sym(str(c->label)));
		break;
	case RTmp:
		assert(isreg(r));
		fprintf(e->f, "\tCALL\t%s\n", rname(r.val));
		break;
	default:
		die("maize emit: unsupported call target");
	}
}

static void
emitins(Ins *i, E *e)
{
	switch (i->op) {
	case Onop:
		break;
	case Ocopy:
		emitcopy(i, e);
		break;
	case Ocall:
		emitcall(i, e);
		break;
	default:
		die("maize emit: unimplemented op '%s' (maize-63)", optab[i->op].name);
	}
}

/* Reserved-frame layout (bytes below BP):
 *   [BP-8*1 .. BP-8*nsaved]   saved callee-saved registers
 *   [.. below ..]             spill slots / locals
 * For hello world nsaved == 0 and there are no locals, so the frame is empty
 * and the prologue/epilogue reduce to exactly asm/hello.mazm:strlen's shape. */
static uint64_t
savedoff(uint n)
{
	return 8 * (n + 1);
}

static void
framelayout(E *e)
{
	int *r;
	uint64_t locals;

	e->nsaved = 0;
	for (r = maize_rclob; *r >= 0; r++)
		e->nsaved += (e->fn->reg >> *r) & 1;

	locals = 4 * (uint64_t)e->fn->slot;
	locals = (locals + 7) & ~(uint64_t)7;   /* 8-byte alignment */
	e->frame = locals + 8 * e->nsaved;
}

static void
prologue(E *e)
{
	int *r;
	uint n;

	fputs("\tPUSH\tBP\n", e->f);
	fputs("\tCP\tSP BP\n", e->f);
	if (e->frame)
		fprintf(e->f, "\tSUB\t$%08"PRIx64" SP\n", e->frame);
	n = 0;
	for (r = maize_rclob; *r >= 0; r++)
		if (e->fn->reg & BIT(*r)) {
			fprintf(e->f, "\tLEA\t$-%02"PRIx64" BP RT\n", savedoff(n));
			fprintf(e->f, "\tST\t%s @RT\n", rname(*r));
			n++;
		}
}

static void
epilogue(E *e)
{
	int *r;
	uint n;

	n = 0;
	for (r = maize_rclob; *r >= 0; r++)
		if (e->fn->reg & BIT(*r)) {
			fprintf(e->f, "\tLEA\t$-%02"PRIx64" BP RT\n", savedoff(n));
			fprintf(e->f, "\tLD\t@RT %s\n", rname(*r));
			n++;
		}
	fputs("\tCP\tBP SP\n", e->f);
	fputs("\tPOP\tBP\n", e->f);
	fputs("\tRET\n", e->f);
}

void
maize_emitfn(Fn *fn, FILE *out)
{
	static int id0;
	E *e;
	Blk *b;
	Ins *i;
	int lbl;

	e = &(E){.f = out, .fn = fn};
	framelayout(e);

	fprintf(e->f, "%s:\n", maize_sym(fn->name));
	prologue(e);

	for (lbl = 0, b = fn->start; b; b = b->link) {
		if (lbl || b->npred > 1)
			fprintf(e->f, "Lm%d:\n", id0 + b->id);
		for (i = b->ins; i != &b->ins[b->nins]; i++)
			emitins(i, e);
		lbl = 1;
		switch (b->jmp.type) {
		case Jret0:
			epilogue(e);
			break;
		case Jjmp:
			if (b->s1 != b->link)
				fprintf(e->f, "\tJMP\tLm%d\n", id0 + b->s1->id);
			else
				lbl = 0;
			break;
		default:
			die("maize emit: unsupported control flow (maize-63)");
		}
	}
	id0 += fn->nblk;
}
