/* maize-63 capstone: full-matrix, self-checking, stdout-observable.
   puts is provided by the freestanding runtime (toolchain/rt), forward-declared
   to satisfy cproc's strict C11 front-end; resolved in mazm's shared label table. */
int puts(const char *);

static int sum_to(int n) {                 /* while-loop, signed add + csle, 2nd call level */
    int s = 0, i = 1;
    while (i <= n) { s = s + i; i = i + 1; }
    return s;                              /* sum_to(10) == 55 */
}

int main(void) {
    int ok = 1;

    if (sum_to(10) != 55) ok = 0;          /* call chain main->sum_to; loop; branch */

    { int a = -20, b = 3;                  /* signed DIV/MOD, trunc toward zero */
      if (a / b != -6) ok = 0;
      if (a % b != -2) ok = 0; }

    { unsigned x = 100u, y = 7u;           /* UDIV/UMOD */
      if (x / y != 14u) ok = 0;
      if (x % y != 2u) ok = 0; }

    { unsigned m = 0xF0u;                   /* AND/OR/XOR, SHR(logical), SHL */
      if ((m & 0x3Cu) != 0x30u) ok = 0;
      if ((m | 0x0Fu) != 0xFFu) ok = 0;
      if ((m ^ 0xFFu) != 0x0Fu) ok = 0;
      if ((m >> 2)    != 0x3Cu) ok = 0;
      if ((m << 1)    != 0x1E0u) ok = 0; }

    { int s = -8;                           /* SAR (arithmetic right shift, maize-54) */
      if ((s >> 1) != -4) ok = 0; }

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
