/* maize-137 float/double (s/d) codegen gate.

   The qbe-maize back-end lowers QBE's Ks/Kd operations to Maize FP instructions
   (Zfinx: floats live in the general register file). This fixture is the primary
   end-to-end gate: it exercises float AND double arithmetic, all six comparisons
   in both widths (ordered and NaN/unordered), int<->float and float<->double
   conversions (32- and 64-bit signed, both directions), inline float/double
   constants, and passing AND returning float/double across a function-call
   boundary.

   Unsigned int->float is intentionally NOT exercised: the back-end's signed-only
   scope (spec section 8) is not the whole story. The pinned qbe core (submodule
   4420727) has no uwtof/ultof op, yet this cproc emits `uwtof` directly rather
   than synthesizing it from signed ops, so an unsigned conversion cannot be
   parsed by qbe without a core change (a submodule bump, out of scope for this
   card). Every signed conversion op the pinned qbe DOES provide (swtof, sltof,
   stosi, dtosi, exts, truncd) is covered below.

   Before this card, compiling ANY float-parameter/return function died at
   `qbe -t maize` with "maize abi: floating-point arguments are not supported";
   fadd()/dadd() below are exactly that load-bearing before/after case, and their
   checked results gate this fixture.

   No %f is printed (the Maize libc printf has no float conversion). Every result
   is checked by value comparison or by exact IEEE bit pattern through a union, so
   a wrong FP encoding changes a checked value and prints "fp: FAIL" instead of
   "fp: PASS". The inexact known-answer bit patterns (0.1+0.2 at both widths) were
   captured from a host IEEE-754 compiler; Maize uses the same binary32/binary64
   round-to-nearest arithmetic, so the bits are identical.

   Self-checking, single line of stdout: "fp: PASS". */

int puts(const char *);

typedef unsigned int   u32;
typedef unsigned long  u64;   /* Maize long is 64-bit */

/* Exact IEEE bit views (store-then-load reinterpret; a wrong encoding shows up
   as wrong bits). */
static u32 fbits(float f)  { union { float f; u32 u; } v; v.f = f; return v.u; }
static u64 dbits(double d) { union { double d; u64 u; } v; v.d = d; return v.u; }

/* Quiet NaNs built from their bit patterns (deterministic, never trapping). */
static float  mkfnan(void) { union { u32 u; float  f; } v; v.u = 0x7FC00000u;             return v.f; }
static double mkdnan(void) { union { u64 u; double d; } v; v.u = 0x7FF8000000000000ul;    return v.d; }

/* Non-inlined optimization barriers. cproc/qbe do no cross-function inlining, so
   a value returned here is opaque to fold.c and forces a real FP instruction
   rather than a compile-time constant. */
static float  opf(float x)  { return x; }
static double opd(double x) { return x; }
static int    opi(int x)    { return x; }
static long   opl(long x)   { return x; }

/* Arithmetic across a call boundary: parameters are opaque registers, so each
   body emits a genuine FADD/FSUB/FMUL/FDIV (single via H0, double whole). These
   are also the load-bearing "float args/returns now compile" case. */
static float  fadd(float a, float b)  { return a + b; }
static float  fsub(float a, float b)  { return a - b; }
static float  fmul(float a, float b)  { return a * b; }
static float  fdiv(float a, float b)  { return a / b; }
static double dadd(double a, double b) { return a + b; }
static double dsub(double a, double b) { return a - b; }
static double dmul(double a, double b) { return a * b; }
static double ddiv(double a, double b) { return a / b; }

/* Immediate-source FADD (Maize CISC: a float constant flows inline). */
static float faddc(float a) { return a + 2.25f; }

int main(void) {
	int fails = 0;
	float a, b, fn;
	double A, B, dn;

	/* --- Arithmetic, exact-representable, both widths (+ - * /) --- */
	if (!(fadd(1.5f, 2.25f) == 3.75f))   fails++;
	if (!(fsub(3.75f, 1.5f) == 2.25f))   fails++;
	if (!(fmul(1.5f, 2.25f) == 3.375f))  fails++;
	if (!(fdiv(3.0f, 2.0f)  == 1.5f))    fails++;
	if (!(dadd(1.5, 2.25)   == 3.75))    fails++;
	if (!(dsub(3.75, 1.5)   == 2.25))    fails++;
	if (!(dmul(1.5, 2.25)   == 3.375))   fails++;
	if (!(ddiv(3.0, 2.0)    == 1.5))     fails++;

	/* Immediate-source arithmetic. */
	if (!(faddc(1.5f) == 3.75f))         fails++;

	/* --- Inexact known-answer (exact IEEE bit pattern, host-captured) --- */
	if (fbits(fadd(0.1f, 0.2f)) != 0x3E99999Au)             fails++;
	if (dbits(dadd(0.1,  0.2))  != 0x3FD3333333333334ul)    fails++;
	/* The famous double inequality, as a runtime FCMP. */
	if (!(dadd(0.1, 0.2) != 0.3))        fails++;
	if (!(dadd(0.1, 0.2) >  0.3))        fails++;

	/* --- Constant materialization (bit-exact) --- */
	if (fbits(opf(1.5f)) != 0x3FC00000u)                    fails++;
	if (dbits(opd(1.5))  != 0x3FF8000000000000ul)           fails++;

	/* --- All six comparisons, ordered, single width --- */
	a = opf(2.0f); b = opf(3.0f);
	if (!(a == a))  fails++;   if (a == b)   fails++;
	if (!(a != b))  fails++;   if (a != a)   fails++;
	if (!(a < b))   fails++;   if (b < a)    fails++;
	if (!(a <= b))  fails++;   if (!(a <= a)) fails++;   if (b <= a) fails++;
	if (!(b > a))   fails++;   if (a > b)    fails++;
	if (!(b >= a))  fails++;   if (!(a >= a)) fails++;   if (a >= b) fails++;

	/* --- All six comparisons, ordered, double width --- */
	A = opd(2.0); B = opd(3.0);
	if (!(A == A))  fails++;   if (A == B)   fails++;
	if (!(A != B))  fails++;   if (A != A)   fails++;
	if (!(A < B))   fails++;   if (B < A)    fails++;
	if (!(A <= B))  fails++;   if (!(A <= A)) fails++;   if (B <= A) fails++;
	if (!(B > A))   fails++;   if (A > B)    fails++;
	if (!(B >= A))  fails++;   if (!(A >= A)) fails++;   if (A >= B) fails++;

	/* Reg-vs-immediate compares (immediate FCMP src and const-dst materialize). */
	if (!(opf(3.0f) > 2.0f))  fails++;
	if (!(opf(2.0f) < 3.0f))  fails++;
	if (!(opd(3.0)  > 2.0))   fails++;
	if (!(opd(2.0)  < 3.0))   fails++;

	/* --- NaN / unordered: the five ordered relations are FALSE, != is TRUE --- */
	fn = mkfnan();
	if (fn == a)     fails++;
	if (!(fn != a))  fails++;
	if (fn < a)      fails++;
	if (fn <= a)     fails++;
	if (fn > a)      fails++;
	if (fn >= a)     fails++;

	dn = mkdnan();
	if (dn == A)     fails++;
	if (!(dn != A))  fails++;
	if (dn < A)      fails++;
	if (dn <= A)     fails++;
	if (dn > A)      fails++;
	if (dn >= A)     fails++;

	/* --- Conversions (32- and 64-bit signed, both directions) --- */
	/* int -> float -> int round-trip recovers the integer (swtof, stosi). */
	{
		int i = opi(1000000);
		float f = (float)i;      /* swtof (Kw->Ks) */
		int j = (int)f;          /* stosi (Ks->Kw) */
		if (j != 1000000)        fails++;
	}
	/* signed int -> double (negative), and double -> signed int. */
	if (!((double)opi(-5) == -5.0))                         fails++;  /* swtof (Kw->Kd) */
	if ((int)opd(1234.0) != 1234)                           fails++;  /* dtosi (Kd->Kw) */
	/* 64-bit signed long -> double -> long round-trip (sltof, dtosi Kl). */
	{
		long x = opl(1000000000000l);   /* 10^12, exactly representable */
		double d = (double)x;           /* sltof (Kl->Kd) */
		long y = (long)d;               /* dtosi (Kd->Kl) */
		if (y != 1000000000000l)        fails++;
	}
	/* signed long -> float (sltof Kl->Ks) and float -> long (stosi Ks->Kl). */
	if (!((float)opl(1000000l) == 1000000.0f))             fails++;
	if ((long)opf(1000000.0f) != 1000000l)                 fails++;
	/* float -> double widen (exts): 1.5f -> 1.5. */
	if (!((double)opf(1.5f) == 1.5))                        fails++;
	/* double -> float narrow (truncd). */
	if (!((float)opd(1.5) == 1.5f))                        fails++;

	puts(fails == 0 ? "fp: PASS" : "fp: FAIL");
	return 0;
}
