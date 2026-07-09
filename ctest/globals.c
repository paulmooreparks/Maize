/* maize-77: file-scope globals across the segmented .mzx data/BSS pipeline.
 *
 * Proves, end to end through cproc -> qbe -t maize -> mazm -c -> mzld -> maize:
 *   - an initialized global of each width (char/short/int/long) reads back its
 *     exact little-endian initializer from a real .data segment (emitnum 1/2/4/8
 *     through the section-routing path);
 *   - a mutable file-scope global is both read and written by main and reflects
 *     the post-write value (data-segment ST/LD round-trip against a global, not a
 *     local);
 *   - a wholly-zero array read BEFORE any write comes back all-zero via load_mzx's
 *     NOBITS zero-fill (SECTION BSS + ZERO N), distinguishing zero from garbage;
 *   - the long carries QBE `align 8`, so its linked address is 8-aligned (honored
 *     ALIGN + mzld section-base alignment). Declared after the sub-8 globals, so a
 *     dropped pad or dropped section max-align leaves it at an odd offset and the
 *     check fails.
 *
 * Output is a fixed sequence of "ok" lines (see globals.expected); any wrong value
 * or a non-zero BSS changes stdout. Only puts is used for output, so no number
 * formatting is needed and the fixture stays within existing isel coverage.
 */

int puts(char const *s);

/* Initialized globals, each a distinct width, declared smallest-first so the long
 * genuinely needs alignment padding. */
char  gc = 'A';                       /* 0x41                */
short gs = 0x1234;                    /* 2-byte initializer  */
int   gi = 0x11223344;               /* 4-byte initializer  */
long  gl = 0x0102030405060708L;      /* 8-byte, align 8     */

/* Mutable file-scope global read AND written by main. */
int counter = 100;

/* Wholly-zero array: a NOBITS BSS contribution, read before any write. */
static int zarr[8];

int
main(void)
{
	int i, sum;

	if (gc == 'A')                   puts("gc ok");      else puts("gc BAD");
	if (gs == 0x1234)                puts("gs ok");      else puts("gs BAD");
	if (gi == 0x11223344)            puts("gi ok");      else puts("gi BAD");
	if (gl == 0x0102030405060708L)   puts("gl ok");      else puts("gl BAD");

	/* NOBITS zero-at-load: read zarr before writing anything into it. */
	sum = 0;
	for (i = 0; i < 8; i++)
		sum += zarr[i];
	if (sum == 0)                    puts("bss zero");   else puts("bss GARBAGE");

	/* Mutable-global round trip in .data: 100 + 23 == 123. */
	counter += 23;
	if (counter == 123)              puts("counter ok"); else puts("counter BAD");

	/* Honored alignment: gl's linked address is 8-aligned. */
	if (((unsigned long)&gl & 7UL) == 0) puts("gl aligned"); else puts("gl UNALIGNED");

	return 0;
}
