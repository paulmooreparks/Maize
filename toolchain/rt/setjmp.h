/* toolchain/rt/setjmp.h -- freestanding <setjmp.h> for the Maize C runtime
 * (maize-94, operator ruling on OQ 9082: minimal setjmp/longjmp IN THIS CARD).
 *
 * The borrowed OpenBSD shell (oksh) is built on a sigsetjmp/siglongjmp error-unwind
 * (genv->jbuf across sh.h/exec.c/expr.c/lex.c/main.c); a recompilation port cannot run
 * without it. setjmp/longjmp are machine-dependent register save/restore, implemented
 * in setjmp.mazm over the Maize calling convention (toolchain/qbe-maize/CALLING-
 * CONVENTION.md): setjmp saves the callee-saved set (R6..R9, BP), the caller's SP, and
 * the return PC into the caller-supplied jmp_buf and returns 0; longjmp restores them
 * and resumes at the saved PC returning val (forced to 1 when 0). sigsetjmp/siglongjmp
 * add the signal-mask save/restore (SYS $0E rt_sigprocmask, maize-174) when savemask is
 * nonzero. POSIX regcomp/regexec (ed's other enabler) stays on maize-243.
 *
 * jmp_buf is a 16-slot (128-byte) unsigned-long array; the layout is a libc-INTERNAL
 * contract shared only between this header and setjmp.mazm (the sigcontext precedent),
 * NOT a cross-file ABI. Slots, in setjmp.mazm order:
 *   [0]=R6 [1]=R7 [2]=R8 [3]=R9 [4]=BP [5]=caller SP [6]=return PC
 *   [7]=savemask flag [8]=saved sigset_t mask  [9..15]=reserved headroom
 */
#ifndef MAIZE_SETJMP_H
#define MAIZE_SETJMP_H

typedef unsigned long jmp_buf[16];
typedef unsigned long sigjmp_buf[16];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

int  sigsetjmp(sigjmp_buf env, int savemask);
void siglongjmp(sigjmp_buf env, int val);

#endif /* MAIZE_SETJMP_H */
