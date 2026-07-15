/* demo_child2.c -- the second borrowed static guest printer for quesOS (maize-24).
 *
 * Identical shape to demo_child1.c but a different message and a different exit
 * status, so the reap loop running it AFTER the first (via the same execve path) is
 * proven by both its output and its distinct recorded status appearing in order. */

int printf(const char *, ...);

int main(void) {
    printf("child two: second guest reporting in\n");
    return 3;
}
