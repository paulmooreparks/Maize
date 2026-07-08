/* C toolchain hello world (maize-62, maize-11 AC 6397 / decision 6635).
 *
 * puts is provided by the freestanding runtime (toolchain/rt), not a hosted
 * libc. cproc is a strict C11 front-end and rejects an implicit function
 * declaration, so puts is forward-declared here; the declaration only satisfies
 * the front-end -- the symbol is still resolved to the runtime's puts in mazm's
 * shared label table. Calling puts (rather than write() directly) exercises a
 * real C-level external call through the ABI (arg-in-R0 + CALL), which is the
 * representative slice. puts appends a newline, so the expected stdout is the
 * greeting followed by '\n'. */
int puts(const char *);

int main(void) {
    puts("Hello, world!");
    return 0;
}
