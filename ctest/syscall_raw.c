/* maize-74 AC 7290: a C fixture calls a RAW syscall stub directly.
 *
 * sys_write is the raw stub (toolchain/rt/syscall.mazm, SYS $01; RET), declared by
 * syscall.h and resolved across the object boundary from syscall.mzo. Writing the
 * bytes straight to fd 1 (stdout) with no errno interpretation exercises the raw
 * layer end to end through the full segmented pipeline
 * (cpp -> cproc -> normalize -> qbe -t maize -> mazm -c -> mzld -> maize).
 * Including syscall.h here also satisfies AC 7295 (the header compiles clean). */
#include "syscall.h"

int main(void) {
    static const char msg[] = "raw syscall stub\n";
    sys_write(1, msg, sizeof msg - 1);
    return 0;
}
