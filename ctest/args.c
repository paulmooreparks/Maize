/* C toolchain argc/argv/envp fixture (maize-60).
 *
 * Exercises the System V-style process-start block the VM builds at the top of RAM
 * and crt0 marshals into main(argc, argv, envp). The argv loop is bounded by argc
 * (so a wrong argc changes the number of printed lines and fails the diff); the envp
 * loop walks to its NULL terminator. argv is walked with a cursor rather than argv[i]
 * indexing so the address math stays add-based (a runtime `argv[i]` lowers to i*8,
 * and this VM's MUL-overflow check divides by the multiplier, faulting when i==0 --
 * an unrelated latent VM bug, out of this card's scope; see the card comment). argc
 * is still load-bearing: it is the loop bound, so a miscount changes the line count.
 *
 * puts is the freestanding runtime's puts and appends the per-line newline, so no
 * itoa/libc is needed (that is maize-76). puts is forward-declared to satisfy the
 * strict-C11 front-end; the symbol resolves to the runtime's puts in mazm's shared
 * label table (same pattern as ctest/hello.c). */
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
