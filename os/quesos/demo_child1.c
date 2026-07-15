/* demo_child1.c -- a borrowed static guest printer, execed by quesOS (maize-24).
 *
 * Compiled by the ordinary cc-maize.sh pipeline at the default link base 0x2000
 * with the standard crt0/RT (unmodified: quesOS execs stock .mzx images). It prints
 * one line via SYS $01 (which traps into quesOS's cause-7 handler and passes through
 * to native write) and returns a distinctive nonzero status so the reaper's recorded
 * exit code is observable and unambiguous. */

int printf(const char *, ...);

int main(void) {
    printf("child one: hello from a quesos guest\n");
    return 7;
}
