/* maize-365: exercise kilo's real syntax highlighter directly against the
 * tab-then-// input that overruns row->hl at kilo.c:485, with no pty and no
 * quesOS. Includes the real kilo.c (kilo's own main is renamed out of the way
 * and never invoked) so this exercises the identical highlighter kilo.mzx
 * runs, not a re-implementation. */
#define main kilo_original_main
#include "../demos/kilo/kilo.c"
#undef main

int main(void) {
    /* Skip initEditor(): it calls updateWindowSize(), which needs a real
     * terminal. Only set the fields editorInsertRow/editorUpdateRow/
     * editorUpdateSyntax actually read. */
    E.numrows = 0;
    E.rowcap = 0;
    E.row = NULL;
    E.syntax = NULL;

    editorSelectSyntaxHighlight("t.c"); /* real function; matches C_HL_extensions */

    /* 'A', TAB, '/', '/'. No trailing newline stored, matching how
     * editorOpen inserts a line with the newline already stripped. */
    editorInsertRow(0, "A\t//", 4);

    erow *row = &E.row[0];
    printf("size=%d rsize=%d hl=", row->size, row->rsize);
    for (int i = 0; i < row->rsize; i++) {
        printf("%d", row->hl[i]);
        if (i + 1 < row->rsize) printf(" ");
    }
    printf("\n");
    printf("OK\n");
    return 0;
}
