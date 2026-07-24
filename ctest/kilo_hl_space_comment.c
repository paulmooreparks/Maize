/* maize-365: the previously-working control for the kilo.c:485 hl-overrun fix.
 * A space-indented // comment ("A //") has no tab, so render equals chars and
 * rsize equals size; the fill count row->size-i and row->rsize-i are the same
 * value here, so this case highlights identically before and after the fix.
 * Pairs with kilo_hl_tab_comment.c (the tab-indented case that actually
 * overran), proving the fix does not change behavior on input that already
 * worked. Same real-source discipline: include the real kilo.c with kilo's own
 * main renamed out of the way and never invoked. */
#define main kilo_original_main
#include "../demos/kilo/kilo.c"
#undef main

int main(void) {
    E.numrows = 0;
    E.rowcap = 0;
    E.row = NULL;
    E.syntax = NULL;

    editorSelectSyntaxHighlight("t.c"); /* real function; matches C_HL_extensions */

    /* 'A', SPACE, '/', '/'. render == chars (no tab), so rsize == size. */
    editorInsertRow(0, "A //", 4);

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
