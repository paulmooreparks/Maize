/* argcheck.c -- maize-359 boot-argv-forwarding fixture, run UNDER quesOS.
 *
 * quesOS's boot forwards the tokens after the image path (argv[1..], split on `--`
 * for multi-program boot) to the launched program as its full argv (path + args).
 * This is the quesOS analog of ctest/args.c: it prints each argv entry on its own
 * line, bounded by argc, then each envp entry to the NULL terminator. The argv block
 * is the argv-forwarding gate (`quesos.mzx /progs/argcheck.mzx a b -c` must arrive as
 * argv == [/progs/argcheck.mzx, a, b, -c]); the envp block is the launcher --env gate
 * (maize-287, preserved by this card: `maize --env K=V ...` must reach the program).
 *
 * Modeled on ctest/args.c: argv is walked with a cursor (not argv[i] indexing) so the
 * address math stays add-based, dodging the same latent MUL-overflow-at-i==0 the
 * ctest/args.c comment documents; argc remains the load-bearing loop bound. puts is
 * the freestanding runtime's puts (appends the per-line newline), forward-declared to
 * satisfy the strict-C11 front-end and resolved out of mazm's shared label table. */
int puts(const char *);

int main(int argc, char **argv, char **envp) {
    char **p = argv;
    for (int i = 0; i < argc; ++i) {   /* one argv entry per line, bounded by argc */
        puts(*p);
        ++p;
    }
    for (char **e = envp; *e; ++e) {    /* one envp entry per line, to the NULL term */
        puts(*e);
    }
    return 0;
}
