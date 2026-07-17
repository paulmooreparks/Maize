/* bin_echoer.c -- maize-94 Phase (b) execvp target. A trivial /bin binary: prints a
 * marker (proving it was found via PATH and exec'd) and exits with a distinctive status
 * the launcher checks through WEXITSTATUS. Built into the /bin mount as bin_echoer.mzx.
 */
int printf(const char *, ...);
int main(void) {
    printf("execvp-target: ran\n");
    return 7;
}
