/* maize-77: pointer-in-data (R_MAIZE_ABS64) end to end through the C pipeline.
 *
 * Each address embedded in initialized data lowers to a QBE `l $sym[+off]` item,
 * which data.c emits as `DREF 8 sym[+off]` (maize-89's directive). mazm -c records
 * an R_MAIZE_ABS64 placeholder + addend; mzld patches each slot with the symbol's
 * linked address plus the addend. If any pointer resolves wrong, stdout changes.
 *
 *   - int *p = &g;         deref yields g's value (a plain pointer-in-data);
 *   - int *q = &arr[1];    deref yields arr[1] (proves the nonzero addend, arr+4);
 *   - char *msgs[] = {...}; each entry printed via the table pointer (proves an
 *     array of R_MAIZE_ABS64 references, and that the string literals routed to
 *     RODATA resolve correctly).
 */

int puts(char const *s);

int  g = 42;
int  arr[3] = { 10, 20, 30 };

int *p = &g;                 /* DREF 8 g            */
int *q = &arr[1];            /* DREF 8 arr+4 (addend) */

char *msgs[] = { "alpha", "bravo" };   /* two DREF 8 into a RODATA string table */

int
main(void)
{
	if (*p == 42)   puts("deref ok");  else puts("deref BAD");
	if (*q == 20)   puts("addend ok"); else puts("addend BAD");
	puts(msgs[0]);
	puts(msgs[1]);
	return 0;
}
