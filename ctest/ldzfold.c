/* maize-204: zero-extending sub-word loads through the real C pipeline.
 *
 * Proves, end to end through cproc -> qbe -t maize -> mazm -c -> mzld -> maize,
 * that the LDZ fold (one instruction replacing the former LD + CPZ pair) is
 * correct for each narrow unsigned width:
 *
 *   - Oloadub: the DOOM r_draw.c motivating pattern dest[i] = colormap[source[i]],
 *     where an unsigned char array read is itself used as an index into a second
 *     array. The zero-extension is LIVE (a wrong extend corrupts the address).
 *   - Oloaduh: an unsigned short read with the high bit set must zero-extend, not
 *     sign-extend, when widened.
 *   - Oloaduw: an unsigned int read of 0x80000000 / 0xFFFFFFFF must zero-extend to
 *     0x0000_0000_8000_0000 / 0x0000_0000_FFFF_FFFF, never sign-extend. This is the
 *     w-class (32-bit) safety case for LDZ's always-full-w0 zero-extend.
 *
 * Output is a fixed sequence of "ok" lines (ldzfold.expected); any wrong zero-
 * extension or read width flips a line to FAIL and changes stdout. Only puts is
 * used, so no number formatting is needed.
 */

int puts(char const *s);

static unsigned char source[8]   = {0, 1, 2, 3, 200, 201, 254, 255};
static unsigned char colormap[256];
static unsigned char dest[8];

static unsigned short u16[4] = {0x0000, 0x1234, 0x8000, 0xFFFF};
static unsigned int   u32[4] = {0u, 0x12345678u, 0x80000000u, 0xFFFFFFFFu};

int
main(void)
{
	int i;

	/* Oloadub LUT chain: build a byte transform, then dest[i] = colormap[source[i]]. */
	for (i = 0; i < 256; i++)
		colormap[i] = (unsigned char)(i ^ 0x55);
	for (i = 0; i < 8; i++)
		dest[i] = colormap[source[i]];
	for (i = 0; i < 8; i++) {
		unsigned char expect = (unsigned char)(source[i] ^ 0x55);
		if (dest[i] != expect) {
			puts("ub FAIL");
			return 1;
		}
	}
	puts("ub ok");

	/* Oloaduh: high-bit-set short must zero-extend when widened. */
	if ((unsigned long)u16[2] == 0x8000UL
	    && (unsigned long)u16[3] == 0xFFFFUL
	    && (unsigned long)u16[1] == 0x1234UL)
		puts("uh ok");
	else
		puts("uh FAIL");

	/* Oloaduw: high-bit-set / all-ones int must zero-extend, not sign-extend. */
	if ((unsigned long)u32[2] == 0x80000000UL)
		puts("uw ok");
	else
		puts("uw FAIL");

	if ((unsigned long)u32[3] == 0xFFFFFFFFUL)
		puts("uw2 ok");
	else
		puts("uw2 FAIL");

	return 0;
}
