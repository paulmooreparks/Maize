/* maize-58 exit-status observability fixture.
 *
 * main returns a fixed nonzero constant so the C-toolchain runner can assert
 * that the maize host process exit status equals it -- a code path distinct
 * from the stdout contract checks (hello / capstone). 42 is chosen to be well
 * clear of the runner's own pipeline error codes (1 = mismatch/stage failure,
 * 2 = environment/setup failure), so an accidental collision can't mask a real
 * regression. The fixture produces no stdout; only $? is under test. This
 * exercises crt0's CP RV R0 + SYS $3C (sys_exit) path end to end. */
int main(void) {
    return 42;
}
