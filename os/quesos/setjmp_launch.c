/* setjmp_launch.c -- maize-94 fixture (operator ruling on OQ 9082), run UNDER quesOS.
 *
 * Proves the in-card setjmp/longjmp/sigsetjmp/siglongjmp (toolchain/rt/setjmp.mazm),
 * the oksh error-unwind enabler, on three points:
 *   (a) setjmp returns 0 on the direct call, then longjmp (through a real call chain)
 *       makes the same setjmp call site return the longjmp value;
 *   (b) a local live in the setjmp frame survives the longjmp;
 *   (c) sigsetjmp(., 1) saves the signal mask, and siglongjmp restores it after the
 *       mask was changed between the two (via SYS $0E rt_sigprocmask, maize-174).
 * Prints "setjmp-launch: PASS" or a FAIL marker naming the failing point.
 */

#include "setjmp.h"
#include "signal.h"
#include "syscall.h"   /* _exit */

int printf(const char *, ...);

static jmp_buf jb;
static sigjmp_buf sjb;

/* A real (non-inlined-away) call chain that longjmps back to main's setjmp. */
static int deep(int x) {
    if (x == 42)
        longjmp(jb, 7);
    return x + 1;
}

int main(void) {
    volatile int local = 12345;
    int r;
    sigset_t want, empty, cur;

    /* (a) + (b): first setjmp returns 0; longjmp via deep() returns 7; local survives. */
    r = setjmp(jb);
    if (r == 0) {
        deep(42);                 /* does not return here */
        printf("setjmp-launch: FAIL no-longjmp\n");
        return 1;
    }
    if (r != 7) {
        printf("setjmp-launch: FAIL value r=%d\n", r);
        return 1;
    }
    if (local != 12345) {
        printf("setjmp-launch: FAIL local=%d\n", local);
        return 1;
    }

    /* (c): set the mask to {SIGINT}, sigsetjmp(.,1) to save it, clear the mask, then
     * siglongjmp: the mask must come back to {SIGINT}. */
    sigemptyset(&want);
    sigaddset(&want, SIGINT);
    sigprocmask(SIG_SETMASK, &want, (sigset_t *)0);

    if (sigsetjmp(sjb, 1) == 0) {
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, (sigset_t *)0);   /* change the mask */
        siglongjmp(sjb, 3);                                 /* must restore {SIGINT} */
        printf("setjmp-launch: FAIL no-siglongjmp\n");
        return 1;
    }

    cur = (sigset_t)0;
    sigprocmask(SIG_BLOCK, (const sigset_t *)0, &cur);      /* read current mask */
    if (cur != want) {
        printf("setjmp-launch: FAIL mask-not-restored (cur=%lu want=%lu)\n",
               (unsigned long)cur, (unsigned long)want);
        return 1;
    }

    printf("setjmp-launch: PASS\n");
    return 0;
}
