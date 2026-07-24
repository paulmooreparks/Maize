/* maize-350 AC3: exercise kilo's shared geometric-growth helper directly.
 *
 * kilo_next_cap() is a pure function (no I/O, no allocation, no dependency on
 * E or terminal state), so it is testable in a plain bare-VM ctest fixture
 * with no PTY. This fixture includes the REAL header the editor uses (not a
 * duplicate) and prints kilo_next_cap(cap, need, floor) over a fixed sequence
 * covering both the 64-row floor and the 4096-byte floor, including the
 * boundary cases need == floor, need == 2*floor, and need just over a
 * power-of-two multiple. The output is byte-compared against the committed
 * ctest/kilo_next_cap.expected. */
#include <stdio.h>
#include "../demos/kilo/kilo_xalloc.h"

static void show(int cap, int need, int floor_cap) {
    printf("%d %d %d -> %d\n", cap, need, floor_cap,
           kilo_next_cap(cap, need, floor_cap));
}

int main(void) {
    /* Row floor (64). */
    show(0, 1, 64);      /* fresh, tiny need: lands on the floor */
    show(0, 64, 64);     /* boundary: need == floor */
    show(0, 65, 64);     /* just over the floor: one doubling */
    show(64, 65, 64);    /* grow from an existing cap */
    show(64, 128, 64);   /* boundary: need == 2*floor */
    show(128, 129, 64);  /* one doubling from 128 */

    /* Abuf floor (4096). */
    show(0, 1, 4096);       /* fresh, tiny need: lands on the floor */
    show(0, 4096, 4096);    /* boundary: need == floor */
    show(0, 4097, 4096);    /* just over the floor: one doubling */
    show(4096, 8192, 4096); /* boundary: need == 2*floor */
    show(4096, 8193, 4096); /* two doublings needed */
    show(8192, 20000, 4096);/* multiple doublings from an existing cap */
    return 0;
}
