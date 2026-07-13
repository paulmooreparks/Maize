/* maize-138: the OTHER translation unit of the multi-file link regression.
 *
 * This file DEFINES the two symbols that multifile_main.c only declares:
 *   - shared_counter, a file-scope global that main extern-references and that this
 *     TU mutates, exercising a cross-object DATA relocation (definition here,
 *     reference in the other object);
 *   - add_and_tag, a global function main calls across the object boundary,
 *     exercising a cross-object CALL relocation.
 *
 * A link that only ever worked for a single self-contained body object would leave
 * main's references to these symbols unresolved, so the fixture passes only when
 * mzld genuinely resolves cross-object symbols against its global table.
 */

int shared_counter = 100;              /* GLOBAL def; extern-referenced from main */

int
add_and_tag(int a, int b)              /* GLOBAL func; called cross-TU from main */
{
	shared_counter += 1;               /* mutates the shared global */
	return a + b;
}
