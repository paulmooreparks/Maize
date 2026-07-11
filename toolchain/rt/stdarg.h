/* toolchain/rt/stdarg.h -- freestanding <stdarg.h> for the Maize C runtime
 * (maize-98).
 *
 * cproc provides the variadic primitives as builtins (scope.c): the type
 * __builtin_va_list and __builtin_va_start / va_arg / va_end / va_copy. This
 * header is the thin freestanding shim mapping the standard names onto them, a
 * companion to stddef.h with no runtime object (decision 7544). cc-maize.sh
 * preprocesses with `-nostdinc -I toolchain/rt`, so `#include <stdarg.h>`
 * resolves here.
 *
 * The va_list ABI (24-byte SysV gp-subset) and the register save area / stack
 * overflow lowering live entirely in the qbe-maize backend (abi.c, emit.c); the
 * front end treats va_list opaquely and only passes a pointer to it.
 */
#ifndef MAIZE_STDARG_H
#define MAIZE_STDARG_H

typedef __builtin_va_list va_list;

#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)
#define va_copy(d, s)      __builtin_va_copy(d, s)

#endif /* MAIZE_STDARG_H */
