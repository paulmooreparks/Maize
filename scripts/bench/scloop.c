/* maize-343 M3 syscall-bound loop: NITERS sys_clock_ms traps, nothing else. */
#include "syscall.h"
#include "stdio.h"
#ifndef NITERS
#define NITERS 3000000
#endif
int main(void){
    unsigned long acc = 0;
    long i;
    for (i = 0; i < NITERS; i++) acc += sys_clock_ms();
    printf("scloop: %d iters acc=%lu\n", (int)NITERS, acc);
    return 0;
}
