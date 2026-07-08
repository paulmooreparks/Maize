/* maize-63 capstone: full-matrix, self-checking, stdout-observable.
   puts is provided by the freestanding runtime (toolchain/rt), forward-declared
   to satisfy cproc's strict C11 front-end; resolved in mazm's shared label table.

   Every ALU/comparison operand is routed through a non-inlined opaque() barrier
   so QBE's constant folder (qbe/fold.c) cannot collapse the checks to compile-time
   constants. cproc/qbe do no cross-function inlining, so a value returned from
   opaque() is opaque to the folder; without this the DIV/MOD/UDIV/UMOD/MUL/AND/OR/
   XOR/SHL/SHR/SAR and SETcc emit paths never appear in the emitted asm (the folder
   rewrites e.g. `if (a/b != -6)` to `if (0)` and deletes the dead body). volatile
   does NOT help here: this qbe's IL carries no volatile marker, so a volatile local
   still folds. The opaque() call boundary is the only reliable barrier. (maize-63
   code-review cycle 1.) */
int puts(const char *);

/* Optimization barriers: not inlined by cproc/qbe, so their results are opaque to
   the constant folder. Keeps the tested arithmetic identical; only changes how the
   operand values are produced (literal -> value returned through an opaque call). */
static int      opaque(int x)       { return x; }
static unsigned opaqueu(unsigned x) { return x; }

static int sum_to(int n) {                 /* while-loop, signed add + csle, 2nd call level */
    int s = 0, i = 1;
    while (i <= n) { s = s + i; i = i + 1; }
    return s;                              /* sum_to(10) == 55 */
}

int main(void) {
    int ok = 1;

    if (sum_to(opaque(10)) != 55) ok = 0;  /* call chain main->sum_to; loop; branch (ADD) */

    { int a = opaque(-20), b = opaque(3);  /* signed DIV/MOD, trunc toward zero */
      if (a / b != -6) ok = 0;             /* DIV  */
      if (a % b != -2) ok = 0; }           /* MOD  */

    { unsigned x = opaqueu(100u), y = opaqueu(7u);  /* UDIV/UMOD */
      if (x / y != 14u) ok = 0;            /* UDIV */
      if (x % y != 2u)  ok = 0; }          /* UMOD */

    { int a = opaque(6), b = opaque(7);    /* MUL and register-operand SUB */
      if (a * b != 42) ok = 0;             /* MUL  */
      if (a - b != -1) ok = 0; }           /* SUB  */

    { unsigned m = opaqueu(0xF0u);         /* AND/OR/XOR, SHR(logical), SHL */
      if ((m & 0x3Cu) != 0x30u)  ok = 0;   /* AND  */
      if ((m | 0x0Fu) != 0xFFu)  ok = 0;   /* OR   */
      if ((m ^ 0xFFu) != 0x0Fu)  ok = 0;   /* XOR  */
      if ((m >> 2)    != 0x3Cu)  ok = 0;   /* SHR  */
      if ((m << 1)    != 0x1E0u) ok = 0; } /* SHL  */

    { int s = opaque(-8);                   /* SAR (arithmetic right shift, maize-54) */
      if ((s >> 1) != -4) ok = 0; }        /* SAR  */

    /* Comparisons materialized as 0/1 values (SETcc, maize-55): each relational
       result feeds integer arithmetic (flags += ...), which forces QBE to lower
       Oflag<cc> to a materialized SETcc rather than fuse it into a branch. Operands
       are opaque so the flags are computed at runtime; signed and unsigned
       conditions are both exercised, distinct from the branch-predicate (Jcc)
       lowering used by the if()s above. */
    { int a = opaque(-5), b = opaque(9);
      unsigned ua = opaqueu(5u), ub = opaqueu(9u);
      int flags = 0;
      flags += (a <  b);                    /* SETLT => 1 */
      flags += (a == b);                    /* SETZ  => 0 */
      flags += (a != b);                    /* SETNZ => 1 */
      flags += (a >= b);                    /* SETGE => 0 */
      flags += (ua <  ub);                  /* SETB  => 1 */
      flags += (ua >  ub);                  /* SETA  => 0 */
      if (flags != 3) ok = 0; }

    { const char *p = "AZ";                 /* loadsb: signed char, pointer arith, string handling */
      if (p[0] != 'A') ok = 0;
      if (p[1] != 'Z') ok = 0; }

    { const unsigned char *q = (const unsigned char *)"\xC8";  /* loadub: zero-extend 0xC8 -> 200 */
      if (q[0] != 200u) ok = 0; }           /* distinguishes zero- from sign-extend (-56) */

    { char buf[3];                          /* Oalloc (frame) + Ostoreb + loadsb round-trip */
      buf[0] = 'x'; buf[1] = 'y'; buf[2] = 0;
      if (buf[0] != 'x' || buf[1] != 'y') ok = 0; }

    puts(ok ? "capstone: PASS" : "capstone: FAIL");   /* 1st call level; ternary => phi/select */
    return 0;
}
