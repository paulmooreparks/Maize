/* maize-138: main TU of the multi-file link regression. It DECLARES (does not
 * define) the two symbols the sibling TU (multifile_lib.c) provides, forcing mzld
 * to resolve a cross-object CALL relocation (add_and_tag) and a cross-object DATA
 * reference (shared_counter). Self-checks to fixed stdout lines (multifile.expected):
 * add_and_tag(40, 2) must return 42, and the shared global, initialized to 100 in the
 * sibling TU and bumped once inside add_and_tag, must read back as 101 from here.
 *
 * cproc is strict C11, so libc-style functions are declared before use; the symbol
 * still resolves to the runtime's definition in the linked image.
 */

int puts(char const *s);

extern int shared_counter;             /* SHN_UNDEF ref  -> resolved to lib's def */
int add_and_tag(int, int);             /* SHN_UNDEF call -> resolved to lib's def */

int
main(void)
{
	int r = add_and_tag(40, 2);        /* cross-object CALL relocation */

	if (r == 42)               puts("call ok");   else puts("call BAD");
	if (shared_counter == 101) puts("shared ok"); else puts("shared BAD");

	return 0;
}
