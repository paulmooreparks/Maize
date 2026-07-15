#include "all.h"

/* Maize assembly emission (maize-63, full single-TU coverage).
 *
 * Emits mazm mnemonics (CP, CALL, ADD, CMP, Jcc, LD, ST, ...), never raw opcode
 * bytes; mazm assembles them to the maize-64 encoding. This covers the full
 * instruction-selection matrix a nontrivial single-TU C program reaches
 * (maize-11 "Back-end instruction-selection coverage"): control flow, the
 * comparison family as branch predicates and as materialized 0/1 values,
 * arithmetic / logic / shift / signed+unsigned div/mod, sign/zero-extending
 * sub-word loads and stores, explicit extensions, and frame-slot addressing.
 * Anything genuinely out of scope (floating point, aggregates, ...) still
 * die()s so it surfaces rather than miscompiling.
 *
 * Width convention (maize-11 decision 6406, the `w`-representation idiom): a QBE
 * `w` (32-bit) value lives in a register's `H0` sub-register and its ALU/CMP ops
 * run at 32-bit width; a QBE `l` (64-bit) value occupies the whole register.
 * clssz() maps the operand class to the mazm sub-register suffix, so every
 * operand is printed at exactly its class width.
 */

typedef struct E E;
struct E {
	FILE *f;
	Fn *fn;
	uint64_t frame;   /* bytes reserved by the prologue SUB */
	uint nsaved;      /* callee-saved registers preserved */
	uint64_t vabase;  /* variadic register save area (48) or 0 (maize-98) */
};

/* Sub-register size codes, low-field first. */
enum { SzB, SzQ, SzH, SzW };
static const char *subsuf[] = { ".B0", ".Q0", ".H0", "" };
static const int   subhex[] = { 2, 4, 8, 16 };

/* Operand width for a QBE class: `w` -> H0 (32-bit), `l` -> whole (64-bit). */
static int
clssz(int cls)
{
	return KWIDE(cls) ? SzW : SzH;
}

static uint64_t
szmask(int sz)
{
	return sz == SzW ? ~(uint64_t)0 : ((uint64_t)1 << (subhex[sz] * 4)) - 1;
}

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

/* Print a value operand (constant immediate or register sub-register) at size
 * sz. Integer constants are masked to the size so a negative value prints at the
 * exact field width the consuming op reads; address constants become a full-64
 * label reference (maize-11 decision 6415), always at whole-register width. */
static void
opnd(Ref r, int sz, E *e)
{
	Con *c;

	switch (rtype(r)) {
	case RCon:
		c = &e->fn->con[r.val];
		switch (c->type) {
		case CBits:
			fprintf(e->f, "$%0*"PRIx64, subhex[sz],
				(uint64_t)c->bits.i & szmask(sz));
			break;
		case CAddr:
			if (c->bits.i != 0)
				die("maize emit: nonzero address offset is not supported");
			fprintf(e->f, "%s", maize_sym(str(c->label)));
			break;
		default:
			die("maize emit: undefined constant");
		}
		break;
	case RTmp:
		assert(isreg(r));
		fprintf(e->f, "%s%s", rname(r.val), subsuf[sz]);
		break;
	default:
		die("maize emit: unsupported operand");
	}
}

/* Print a register destination at size sz. */
static void
regw(Ref r, int sz, E *e)
{
	assert(isreg(r));
	fprintf(e->f, "%s%s", rname(r.val), subsuf[sz]);
}

static void lea_negoff(E *e, uint64_t off, const char *reg);
static void lea_off(E *e, int64_t off, const char *reg);

/* Byte offset below BP of frame slot s (a 4-byte unit index): the spill/local
 * region sits just below the saved-register block (8*nsaved) and, for a variadic
 * function, the register save area (vabase). Shared verbatim by emitaddrslot(),
 * memaddrreg()'s slot case (reload / spill store), and emitcopy()'s slot cases
 * so a given slot always resolves to the identical address regardless of which
 * instruction form references it. */
static uint64_t
slotoff(E *e, int s)
{
	return e->vabase + 8 * (uint64_t)e->nsaved + 4 * ((uint64_t)e->fn->slot - s);
}

/* Resolve a memory-address operand to a register for `@reg` addressing, and
 * return that register's name. A register address is used directly. A label
 * (or other constant) address is first materialized into the RT scratch with a
 * preceding `CP <label> RT`; the caller relies on this being emitted BEFORE the
 * LD/ST line it is building. Maize keeps memory addresses in registers on both
 * sides of a memory op (maize-43): mazm rejects `ST src @label` outright, and
 * its `LD @label` ties the address-immediate width to the loaded sub-register
 * (so a `char` global's address would truncate to ABS8). Routing every label
 * address through RT sidesteps both and matches the register-address idiom the
 * frame-slot loads/stores already use. */
static const char *
memaddrreg(Ref r, E *e)
{
	Con *c;

	switch (rtype(r)) {
	case RTmp:
		assert(isreg(r));
		return rname(r.val);
	case RCon:
		c = &e->fn->con[r.val];
		if (c->type != CAddr || c->bits.i != 0)
			die("maize emit: unsupported memory address");
		fprintf(e->f, "\tCP\t%s RT\n", maize_sym(str(c->label)));
		return "RT";
	case RSlot:
		/* Spill reload (Oload) / spill store (Ostore) address a frame slot:
		 * materialize its address in the RT scratch, then LD/ST @RT. RT is the
		 * rglob scratch and never holds a live value, so it is free here. */
		lea_negoff(e, slotoff(e, r.val), "RT");
		return "RT";
	default:
		die("maize emit: unsupported memory address");
	}
	return 0;
}

/* CP <src> <dst> at size sz. */
static void
cp(Ref src, Ref dst, int sz, E *e)
{
	fputs("\tCP\t", e->f);
	opnd(src, sz, e);
	fputc(' ', e->f);
	regw(dst, sz, e);
	fputc('\n', e->f);
}

/* <mnem> <src at ssz> <dst at dsz>. */
static void
alu(const char *mnem, Ref src, int ssz, Ref dst, int dsz, E *e)
{
	fprintf(e->f, "\t%s\t", mnem);
	opnd(src, ssz, e);
	fputc(' ', e->f);
	regw(dst, dsz, e);
	fputc('\n', e->f);
}

/* Ocopy lowering, including spilled (RSlot) src and/or dst. Because
 * maize_memargs() returns 0, a slot never appears inline in any other operand,
 * so a slot reaches emit only here and as a load/store address. A same-ref copy
 * is a no-op. Otherwise the cases are:
 *   reg / CBits / CAddr -> reg : plain CP (unchanged path).
 *   slot               -> reg : LEA the slot into RT, then LD @RT into the reg.
 *   reg / CBits        -> slot: LEA the dst slot into RT, then ST the value @RT.
 *   CAddr (label)      -> slot: borrow R0 (PUSH/POP) to hold the whole-register
 *                               label, then ST it @RT (mazm rejects a bare label
 *                               as a store immediate; decision 6415).
 *   slot               -> slot: borrow R0 (PUSH/POP) as the value register while
 *                               RT carries the address; LD src @RT, then ST @RT.
 * The mem-to-mem and label->slot cases need a value register and an address
 * register at once, which the single RT scratch cannot supply, so one
 * allocatable register (R0) is borrowed across the move. The borrow is correct
 * irrespective of R0's liveness: it is saved and restored, and PUSH/POP move SP,
 * never BP, so every BP-relative slot offset is invariant across the borrow. */
static void
emitcopy(Ins *i, E *e)
{
	Ref src, dst, rb;
	Con *c;
	int sz;

	src = i->arg[0];
	dst = i->to;
	if (req(dst, src))
		return;
	sz = clssz(i->cls);
	rb = TMP(R0);   /* borrowed value register for mem-to-mem / label->slot */

	if (rtype(dst) != RSlot) {
		assert(isreg(dst));
		if (rtype(src) == RSlot) {
			lea_negoff(e, slotoff(e, src.val), "RT");
			fputs("\tLD\t@RT ", e->f);
			regw(dst, sz, e);
			fputc('\n', e->f);
		} else if (rtype(src) == RCon
		&& (c = &e->fn->con[src.val])->type == CAddr
		&& c->bits.i != 0) {
			/* `$sym + K` -> reg (the isel-routed Ocopy): materialize the label
			 * with the zero-offset idiom, then add the byte offset with a
			 * FLAG-NEUTRAL LEA. opnd()/cp() cannot print the nonzero offset, so
			 * emit the label CP directly here. Never ADD/SUB: this copy can land
			 * at a block end after a fused flag-only CMP and before the block
			 * Jcc (successor-phi-arg pass), where a flag write is a silent
			 * miscompile. The isel copy is class Kl, so sz is whole-register. */
			fprintf(e->f, "\tCP\t%s ", maize_sym(str(c->label)));
			regw(dst, sz, e);
			fputc('\n', e->f);
			lea_off(e, c->bits.i, rname(dst.val));
		} else {
			cp(src, dst, sz, e);
		}
		return;
	}

	/* Destination is a spill slot; address it in RT and store the value. */
	switch (rtype(src)) {
	case RTmp:
		assert(isreg(src));
		lea_negoff(e, slotoff(e, dst.val), "RT");
		fputs("\tST\t", e->f);
		opnd(src, sz, e);
		fputs(" @RT\n", e->f);
		break;
	case RCon:
		c = &e->fn->con[src.val];
		if (c->type == CAddr) {
			fprintf(e->f, "\tPUSH\t%s\n", rname(R0));
			if (c->bits.i != 0) {
				/* `$sym + K` -> slot: rega spilled the isel-routed copy temp,
				 * so its destination became a frame slot. Materialize the label
				 * into the borrowed R0 then add K with the FLAG-NEUTRAL LEA
				 * (never ADD/SUB), before storing R0 to the slot. cp()/opnd()
				 * cannot print the nonzero offset, so emit the label CP here. */
				fprintf(e->f, "\tCP\t%s ", maize_sym(str(c->label)));
				regw(rb, sz, e);
				fputc('\n', e->f);
				lea_off(e, c->bits.i, rname(R0));
			} else {
				cp(src, rb, sz, e);
			}
			lea_negoff(e, slotoff(e, dst.val), "RT");
			fputs("\tST\t", e->f);
			opnd(rb, sz, e);
			fputs(" @RT\n", e->f);
			fprintf(e->f, "\tPOP\t%s\n", rname(R0));
			break;
		}
		/* CBits immediate stores directly (ST immVal regAddr). */
		lea_negoff(e, slotoff(e, dst.val), "RT");
		fputs("\tST\t", e->f);
		opnd(src, sz, e);
		fputs(" @RT\n", e->f);
		break;
	case RSlot:
		fprintf(e->f, "\tPUSH\t%s\n", rname(R0));
		lea_negoff(e, slotoff(e, src.val), "RT");
		fputs("\tLD\t@RT ", e->f);
		regw(rb, sz, e);
		fputc('\n', e->f);
		lea_negoff(e, slotoff(e, dst.val), "RT");
		fputs("\tST\t", e->f);
		opnd(rb, sz, e);
		fputs(" @RT\n", e->f);
		fprintf(e->f, "\tPOP\t%s\n", rname(R0));
		break;
	default:
		die("maize emit: unsupported copy source");
	}
}

/* XCHG ra rb: exchange two physical registers whole (maize $E0). rega's pmgen
 * emits Oswap for a register cycle in a phi / parallel move (the &&/||/ternary
 * fixtures); after rega both operands are physical registers. pmrec widens any
 * cycle containing an `l` to class Kl, and XCHG exchanges whole registers, which
 * is correct for Kl and acceptable for a pure-Kw cycle (a `w` value's upper bits
 * are don't-care under the maize w-representation, decision 6406), so the swap is
 * emitted at whole-register width with no sub-suffix. */
static void
emitswap(Ins *i, E *e)
{
	assert(isreg(i->arg[0]) && isreg(i->arg[1]));
	fprintf(e->f, "\tXCHG\t%s %s\n", rname(i->arg[0].val), rname(i->arg[1].val));
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

/* Reconcile a 3-address QBE ALU op `to = arg0 OP arg1` to Maize's two-address
 * dst-accumulate form `OP arg1 to` (decision 6780). Establish to == arg0, then
 * apply arg1. When arg1 already aliases `to` for a non-commutative op, route
 * through the RT scratch so arg0 is not clobbered before arg1 is read. Shifts
 * carry a `w` count regardless of the value class, so the source (count) is
 * printed at 32-bit width. */
static void
emitbinop(Ins *i, const char *mnem, int commutative, int shiftlike, E *e)
{
	int dsz, ssz;
	Ref to, a0, a1;

	to = i->to;
	a0 = i->arg[0];
	a1 = i->arg[1];
	assert(isreg(to));
	dsz = clssz(i->cls);
	ssz = shiftlike ? clssz(Kw) : dsz;

	if (req(to, a1) && !commutative) {
		/* to holds arg1; compute in RT so arg0 survives. */
		cp(a0, TMP(RT), dsz, e);
		alu(mnem, a1, ssz, TMP(RT), dsz, e);
		cp(TMP(RT), to, dsz, e);
		return;
	}
	if (req(to, a1)) {
		/* commutative: to holds arg1, so `OP arg0 to`. */
		alu(mnem, a0, dsz, to, dsz, e);
		return;
	}
	if (!req(to, a0))
		cp(a0, to, dsz, e);
	alu(mnem, a1, ssz, to, dsz, e);
}

/* Condition (CmpI index) -> Jcc / SETcc mnemonic. Mirrors the shared predicate
 * table in src/maize_cpu.h (decision 6776): ieq JZ, ine JNZ, isge JGE, isgt JGT,
 * isle JLE, islt JLT, iuge JAE, iugt JA, iule JBE, iult JB. */
static const char *jcctab[NCmpI] = {
	"JZ", "JNZ", "JGE", "JGT", "JLE", "JLT", "JAE", "JA", "JBE", "JB"
};
static const char *setcctab[NCmpI] = {
	"SETZ", "SETNZ", "SETGE", "SETGT", "SETLE", "SETLT",
	"SETAE", "SETA", "SETBE", "SETB"
};

/* CMP arg1 arg0 (== arg0 - arg1, so the flags match QBE's `arg0 <cc> arg1`),
 * then SETcc into `to` when the result is materialized as a value. When to == R
 * the comparison is flag-only: seljmp fused it into the block's Jcc. */
static void
emitcmp(Ins *i, int kc, int c, E *e)
{
	int sz;

	sz = clssz(kc);
	fputs("\tCMP\t", e->f);
	opnd(i->arg[1], sz, e);    /* src */
	fputc(' ', e->f);
	regw(i->arg[0], sz, e);    /* dst (register) */
	fputc('\n', e->f);
	if (!req(i->to, R))
		/* Bare register destination writes a clean full-W0 0/1. */
		fprintf(e->f, "\t%s\t%s\n", setcctab[c], rname(i->to.val));
}

/* Float compare (maize-137). Emit `FCMP <src> <dst(reg)>` (same operand order
 * as the integer CMP: dst = arg0 = `a`, src = arg1), which sets Maize's flags
 * byte-identically to x86 UCOMISD (a>src => C0Z0P0, a<src => C1Z0P0, a==src =>
 * C0Z1P0, unordered => C1Z1P1; P is the unordered/NaN indicator). When to == R
 * the compare is flag-only (seljmp fused it into the block's Jcc). Otherwise
 * materialize a clean 0/1 into `to` per the canonical relation. isel canonicali-
 * zes lt/le into gt/ge, so the surviving value relations are exactly {gt, ge,
 * eq, ne, o, uo}. SETA/SETAE/SETP each write a clean full-W0 0/1 (bare register
 * dst); the byte-wise fixups touch only .B0 and leave the upper bytes zero, so
 * the result is a clean 0/1. RT is the reserved back-end scratch. */
static void
emitfcmp(Ins *i, int kc, int c, E *e)
{
	int sz;
	const char *dr;

	sz = clssz(kc);
	fputs("\tFCMP\t", e->f);
	opnd(i->arg[1], sz, e);    /* src */
	fputc(' ', e->f);
	regw(i->arg[0], sz, e);    /* dst (register) = a */
	fputc('\n', e->f);
	if (req(i->to, R))
		return;
	dr = rname(i->to.val);
	switch (c) {
	case NCmpI+Cfgt:   /* a > src, ordered: A = !C && !Z */
		fprintf(e->f, "\tSETA\t%s\n", dr);
		break;
	case NCmpI+Cfge:   /* a >= src, ordered: AE = !C */
		fprintf(e->f, "\tSETAE\t%s\n", dr);
		break;
	case NCmpI+Cfuo:   /* unordered: P */
		fprintf(e->f, "\tSETP\t%s\n", dr);
		break;
	case NCmpI+Cfo:    /* ordered = !P (Maize has no SETNP) */
		fprintf(e->f, "\tSETP\t%s\n", dr);
		fprintf(e->f, "\tXOR\t$01 %s.B0\n", dr);
		break;
	case NCmpI+Cfeq:   /* equal-ordered = Z && !P */
		fprintf(e->f, "\tSETZ\t%s\n", dr);
		fprintf(e->f, "\tSETP\tRT\n");
		fprintf(e->f, "\tXOR\t$01 RT.B0\n");
		fprintf(e->f, "\tAND\tRT.B0 %s.B0\n", dr);
		break;
	case NCmpI+Cfne:   /* C != is TRUE when unordered: !Z || P */
		fprintf(e->f, "\tSETNZ\t%s\n", dr);
		fprintf(e->f, "\tSETP\tRT\n");
		fprintf(e->f, "\tOR\tRT.B0 %s.B0\n", dr);
		break;
	default:
		die("maize emit: unsupported float compare %d", c);
	}
}

/* Sub-word loads: the zero-extend forms (Oloadub/uh/uw) are a single LDZ (card
 * maize-204, folding the former LD + CPZ pair; decision 6779 "there is no LDZ" is
 * superseded by the operator ruling on card maize-204). The sign-extend forms
 * (Oloadsb/sh/sw) stay LD + CP; the full-width form (Oload) is a plain LD. */
static void
emitload(Ins *i, E *e)
{
	int rsz, signext, dsz;

	switch (i->op) {
	case Oloadsb: rsz = SzB; signext = 1; break;
	case Oloadub: rsz = SzB; signext = 0; break;
	case Oloadsh: rsz = SzQ; signext = 1; break;
	case Oloaduh: rsz = SzQ; signext = 0; break;
	case Oloadsw: rsz = SzH; signext = 1; break;
	case Oloaduw: rsz = SzH; signext = 0; break;
	case Oload:   rsz = clssz(i->cls); signext = -1; break;
	default: die("maize emit: unsupported load");
	}
	if (signext == 0) {
		/* Zero-extending sub-word load (maize-204): LDZ folds the LD + CPZ pair
		 * into one instruction. LDZ reads rsz bytes and zero-extends into the
		 * FULL destination register regardless of the class width named here;
		 * see the VM-side note (src/cpu.cpp, copy_regaddr_reg_zext) for why that
		 * is safe for the w-class (32-bit) case too. */
		fprintf(e->f, "\tLDZ\t@%s ", memaddrreg(i->arg[0], e));
		regw(i->to, rsz, e);
		fputc('\n', e->f);
		return;
	}

	fprintf(e->f, "\tLD\t@%s ", memaddrreg(i->arg[0], e));
	regw(i->to, rsz, e);
	fputc('\n', e->f);
	if (signext == 1) {
		dsz = clssz(i->cls);
		fprintf(e->f, "\tCP\t");
		regw(i->to, rsz, e);
		fputc(' ', e->f);
		regw(i->to, dsz, e);
		fputc('\n', e->f);
	}
}

/* ST src @addr at the store's width. */
static void
emitstore(Ins *i, E *e)
{
	int ssz;
	const char *areg;

	switch (i->op) {
	case Ostoreb: ssz = SzB; break;
	case Ostoreh: ssz = SzQ; break;
	case Ostorew: ssz = SzH; break;
	case Ostorel: ssz = SzW; break;
	case Ostores: ssz = SzH; break;   /* single-float: 32-bit H0 */
	case Ostored: ssz = SzW; break;   /* double-float: whole register */
	default: die("maize emit: unsupported store");
	}
	areg = memaddrreg(i->arg[1], e);   /* may emit `CP <label> RT` first */
	fputs("\tST\t", e->f);
	opnd(i->arg[0], ssz, e);
	fprintf(e->f, " @%s\n", areg);
}

/* Explicit width cast: CP (sign) / CPZ (zero) from the sub-word source. */
static void
emitext(Ins *i, E *e)
{
	int ssz, signext, dsz;

	switch (i->op) {
	case Oextsb: ssz = SzB; signext = 1; break;
	case Oextub: ssz = SzB; signext = 0; break;
	case Oextsh: ssz = SzQ; signext = 1; break;
	case Oextuh: ssz = SzQ; signext = 0; break;
	case Oextsw: ssz = SzH; signext = 1; break;
	case Oextuw: ssz = SzH; signext = 0; break;
	default: die("maize emit: unsupported extension");
	}
	dsz = clssz(i->cls);
	fprintf(e->f, "\t%s\t", signext ? "CP" : "CPZ");
	opnd(i->arg[0], ssz, e);
	fputc(' ', e->f);
	regw(i->to, dsz, e);
	fputc('\n', e->f);
}

/* Float conversion (maize-137): `<MNEMONIC> <src at ssz> <dst at dsz>`, source
 * first. The source class comes from the op's signature (argcls(i,0), which the
 * reclass pre-pass never touches since it keys on Ins.cls), the dest width from
 * the instruction class. FCVTFF widens/narrows single<->double; FCVTFS is the
 * SIGNED float->int narrow; FCVTSF the SIGNED int->float widen. Unsigned
 * conversions are synthesized in the front end, so no unsigned form is emitted. */
static void
emitfcvt(Ins *i, const char *mnem, E *e)
{
	int ssz, dsz;

	ssz = clssz(argcls(i, 0));
	dsz = clssz(i->cls);
	fprintf(e->f, "\t%s\t", mnem);
	opnd(i->arg[0], ssz, e);
	fputc(' ', e->f);
	regw(i->to, dsz, e);
	fputc('\n', e->f);
}

/* Emit `LEA $-<off> BP <reg>` with the offset immediate sized so mazm reads it at
 * a width that can hold -off. mazm sizes a hex literal by its digit count
 * (mazm.cpp compile_hex_literal): <=2 digits is an 8-bit immediate, whose signed
 * range is only -128..127. A frame offset >= 128 (reachable once the 48-byte
 * variadic save area shifts the saved-register and locals blocks down, maize-98)
 * printed as 2 digits would be truncated to 8 bits and sign-extended to the wrong
 * value, so pad the hex to a width whose signed range holds -off. Offsets < 128
 * still print as 2 digits, keeping non-variadic frame encodings byte-identical. */
static void
lea_negoff(E *e, uint64_t off, const char *reg)
{
	int digits;

	if (off <= 0x7f)
		digits = 2;
	else if (off <= 0x7fff)
		digits = 4;
	else if (off <= 0x7fffffff)
		digits = 8;
	else
		digits = 16;
	fprintf(e->f, "\tLEA\t$-%0*"PRIx64" BP %s\n", digits, off, reg);
}

/* Add a signed constant offset to a whole register in place, FLAG-NEUTRALLY:
 * `LEA $<off> reg reg` (reg = off + reg). LEA is flag-neutral (src/cpu.cpp,
 * instr::lea_immVal_regreg saves/restores RF around its internal add; maize-4),
 * so this is safe to emit in ANY position, including the successor-phi-arg pass,
 * which lands the materialization at block end after a fused flag-only CMP and
 * before the block Jcc (isel.c). ADD/SUB would clobber RF there, a silent
 * miscompile. The offset immediate is digit-sized exactly like lea_negoff so
 * mazm does not sign-extend it to the wrong value (a 2-digit $c8 reads as -56,
 * not +200): pad the magnitude so its top bit is clear. A negative offset uses
 * the same instruction with a negated immediate, `LEA $-<|off|> reg reg`. */
static void
lea_off(E *e, int64_t off, const char *reg)
{
	uint64_t mag;
	int digits;
	const char *sign;

	if (off == 0)
		return;
	sign = off < 0 ? "-" : "";
	mag  = off < 0 ? (uint64_t)-off : (uint64_t)off;
	if (mag <= 0x7f)
		digits = 2;
	else if (mag <= 0x7fff)
		digits = 4;
	else if (mag <= 0x7fffffff)
		digits = 8;
	else
		digits = 16;
	fprintf(e->f, "\tLEA\t$%s%0*"PRIx64" %s %s\n", sign, digits, mag, reg, reg);
}

/* Frame-slot address materialization: LEA $-<off> BP <reg>. The locals region
 * sits just below the saved-register block the prologue reserved; slot s (a
 * 4-byte unit index) is at BP - (vabase + 8*nsaved + 4*(fn->slot - s)). For a
 * variadic function vabase == 48 shifts everything below the register save area
 * that occupies BP-48..BP-0 (maize-98). */
static void
emitaddrslot(Ins *i, E *e)
{
	assert(rtype(i->arg[0]) == RSlot);
	lea_negoff(e, slotoff(e, i->arg[0].val), rname(i->to.val));
}

static void
emitins(Ins *i, E *e)
{
	int kc, c;

	switch (i->op) {
	case Onop:  break;
	case Ocopy: emitcopy(i, e); break;
	case Oswap: emitswap(i, e); break;
	case Ocall: emitcall(i, e); break;
	case Oaddr: emitaddrslot(i, e); break;
	/* Arithmetic dispatches on KBASE(cls): a float op (Ks/Kd) emits the FP
	 * mnemonic, an int op (Kw/Kl) the integer one. The dispatch diverges ONLY
	 * on KBASE(cls)==1, so integer output is byte-identical to before. FADD /
	 * FMUL are commutative, FSUB / FDIV are not (like their int siblings). */
	case Oadd:  emitbinop(i, KBASE(i->cls) ? "FADD" : "ADD",  1, 0, e); break;
	case Osub:  emitbinop(i, KBASE(i->cls) ? "FSUB" : "SUB",  0, 0, e); break;
	case Omul:  emitbinop(i, KBASE(i->cls) ? "FMUL" : "MUL",  1, 0, e); break;
	case Oand:  emitbinop(i, "AND",  1, 0, e); break;
	case Oor:   emitbinop(i, "OR",   1, 0, e); break;
	case Oxor:  emitbinop(i, "XOR",  1, 0, e); break;
	case Oshl:  emitbinop(i, "SHL",  0, 1, e); break;
	case Oshr:  emitbinop(i, "SHR",  0, 1, e); break;
	case Osar:  emitbinop(i, "SAR",  0, 1, e); break;
	case Odiv:  emitbinop(i, KBASE(i->cls) ? "FDIV" : "DIV",  0, 0, e); break;
	case Orem:  emitbinop(i, "MOD",  0, 0, e); break;
	case Oudiv: emitbinop(i, "UDIV", 0, 0, e); break;
	case Ourem: emitbinop(i, "UMOD", 0, 0, e); break;
	case Oexts:
	case Otruncd: emitfcvt(i, "FCVTFF", e); break;
	case Ostosi:
	case Odtosi:  emitfcvt(i, "FCVTFS", e); break;
	case Oswtof:
	case Osltof:  emitfcvt(i, "FCVTSF", e); break;
	default:
		if (iscmp(i->op, &kc, &c)) {
			if (c < NCmpI)
				emitcmp(i, kc, c, e);
			else
				emitfcmp(i, kc, c, e);
			break;
		}
		if (isload(i->op))         { emitload(i, e); break; }
		if (isstore(i->op))        { emitstore(i, e); break; }
		if (isext(i->op))          { emitext(i, e); break; }
		die("maize emit: unimplemented op '%s'", optab[i->op].name);
	}
}

/* Reserved-frame layout (bytes below BP):
 *   [BP-48 .. BP-0]           variadic register save area (only when fn->vararg)
 *   [BP-vabase-8*1 .. ]       saved callee-saved registers
 *   [.. below ..]             spill slots / locals
 * A non-variadic leaf with no saves and no locals has an empty frame, so the
 * prologue/epilogue reduce to exactly asm/hello.mazm:strlen's shape. vabase is 48
 * for a variadic function (the R0..R5 save area at BP-48..BP-0) else 0, and it
 * shifts the saved-register and locals blocks down so the save-area base is the
 * constant BP-48 selvastart hardcodes (maize-98, decisions 7537/7598). */
static uint64_t
savedoff(E *e, uint n)
{
	return e->vabase + 8 * (n + 1);
}

static void
framelayout(E *e)
{
	int *r;
	uint64_t locals;

	e->vabase = e->fn->vararg ? 48 : 0;

	e->nsaved = 0;
	for (r = maize_rclob; *r >= 0; r++)
		e->nsaved += (e->fn->reg >> *r) & 1;

	locals = 4 * (uint64_t)e->fn->slot;
	locals = (locals + 7) & ~(uint64_t)7;   /* 8-byte alignment */
	e->frame = locals + 8 * e->nsaved + e->vabase;
}

static void
prologue(E *e)
{
	int *r;
	uint n;
	int k;

	fputs("\tPUSH\tBP\n", e->f);
	fputs("\tCP\tSP BP\n", e->f);
	if (e->frame)
		fprintf(e->f, "\tSUB\t$%08"PRIx64" SP\n", e->frame);
	/* Variadic register save area (decisions 7537/7598): spill all six GP
	 * argument registers into BP-48..BP-0 (R0 at the lowest address, R5 at BP-8)
	 * before the body can clobber them, so va_start / va_arg can walk them. */
	if (e->fn->vararg)
		for (k = 0; k < 6; k++) {
			lea_negoff(e, 48 - 8 * k, "RT");
			fprintf(e->f, "\tST\t%s @RT\n", rname(R0 + k));
		}
	n = 0;
	for (r = maize_rclob; *r >= 0; r++)
		if (e->fn->reg & BIT(*r)) {
			lea_negoff(e, savedoff(e, n), "RT");
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
			lea_negoff(e, savedoff(e, n), "RT");
			fprintf(e->f, "\tLD\t@RT %s\n", rname(*r));
			n++;
		}
	fputs("\tCP\tBP SP\n", e->f);
	fputs("\tPOP\tBP\n", e->f);
	fputs("\tRET\n", e->f);
}

/* Declare every external symbol the function references (maize-71). Under
 * mazm's strict object model an undefined reference must be declared EXTERN or
 * it is an assembly-time error, so the runtime's cross-object CALLs (crt0 ->
 * main, puts -> sys_write, and the compiled body -> puts) need import
 * declarations. Every symbol reference reaches the emitter as a CAddr constant
 * in an instruction operand (call target or data-symbol address), so scan both
 * args of every instruction and emit `EXTERN <sym>` for each distinct global
 * symbol. Local (block) labels are skipped. EXTERN of a symbol the module also
 * defines is a harmless no-op in mazm (decision 7273), and EXTERN is inert in
 * mazm's flat mode, so the declarations never change flat output. */
static void
emit_externs(E *e)
{
	Blk *b;
	Ins *i;
	Con *c;
	Ref r;
	uint32_t *seen, lbl;
	uint nseen, cap, k;
	int a;

	seen = 0;
	nseen = 0;
	cap = 0;
	for (b = e->fn->start; b; b = b->link)
		for (i = b->ins; i != &b->ins[b->nins]; i++)
			for (a = 0; a < 2; a++) {
				r = i->arg[a];
				if (rtype(r) != RCon)
					continue;
				c = &e->fn->con[r.val];
				if (c->type != CAddr || c->local)
					continue;
				lbl = c->label;
				for (k = 0; k < nseen; k++)
					if (seen[k] == lbl)
						break;
				if (k < nseen)
					continue;
				if (nseen == cap) {
					cap = cap ? cap * 2 : 8;
					seen = realloc(seen, cap * sizeof *seen);
				}
				seen[nseen++] = lbl;
				fprintf(e->f, "EXTERN %s\n", maize_sym(str(lbl)));
			}
	free(seen);
}

void
maize_emitfn(Fn *fn, FILE *out)
{
	static int id0;
	E *e;
	Blk *b;
	Ins *i;
	int c;

	e = &(E){.f = out, .fn = fn};
	framelayout(e);

	/* Segmented pipeline (maize-77): route the function into the CODE section
	 * and export it when QBE marks it visible, so the runtime's cross-object
	 * CALL (e.g. crt0 -> main) resolves through mzld. Both directives are inert
	 * no-ops in mazm's flat mode (decision 7167). */
	fputs("SECTION CODE\n", e->f);
	if (fn->export)
		fprintf(e->f, "GLOBAL %s\n", maize_sym(fn->name));
	emit_externs(e);
	fprintf(e->f, "%s:\n", maize_sym(fn->name));
	prologue(e);

	for (b = fn->start; b; b = b->link) {
		if (b != fn->start)
			fprintf(e->f, "Lm%d:\n", id0 + b->id);
		for (i = b->ins; i != &b->ins[b->nins]; i++)
			emitins(i, e);
		switch (b->jmp.type) {
		case Jret0:
			epilogue(e);
			break;
		case Jjmp:
			if (b->s1 != b->link)
				fprintf(e->f, "\tJMP\tLm%d\n", id0 + b->s1->id);
			break;
		case Jhlt:
			fputs("\tHALT\n", e->f);
			break;
		default:
			if (b->jmp.type >= Jjf && b->jmp.type < Jjf + NCmp) {
				const char *jcc;
				c = b->jmp.type - Jjf;
				if (c < NCmpI)
					jcc = jcctab[c];
				else switch (c - NCmpI) {
					/* Only the three single-Jcc float relations
					 * are ever produced fused (isel seljmp). */
					case Cfgt: jcc = "JA";  break;
					case Cfge: jcc = "JAE"; break;
					case Cfuo: jcc = "JP";  break;
					default:
						die("maize emit: unfusable float branch (maize-137)");
					}
				fprintf(e->f, "\t%s\tLm%d\n",
					jcc, id0 + b->s1->id);
				if (b->s2 != b->link)
					fprintf(e->f, "\tJMP\tLm%d\n",
						id0 + b->s2->id);
			} else
				die("maize emit: unsupported control flow (maize-63)");
			break;
		}
	}
	id0 += fn->nblk;
}
