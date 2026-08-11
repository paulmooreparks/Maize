/* maize-343 M3 compute-bound contrast loop: no syscalls, pure arithmetic. */
#include "stdio.h"
#ifndef NITERS
#define NITERS 100000000
#endif
int main(void){
    unsigned long acc = 1;
    long i;
    for (i = 0; i < NITERS; i++) acc = acc * 1103515245UL + 12345UL + (unsigned long)i;
    printf("compute: %d iters acc=%lu\n", (int)NITERS, acc);
    return 0;
}
