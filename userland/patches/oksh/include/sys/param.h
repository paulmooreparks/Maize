/*
 * sys/param.h (maize-94): Maize-local shim. oksh's portable.h includes
 * <sys/param.h> unconditionally. Wave-1 oksh uses PATH_MAX (from <limits.h>, RT
 * provides it) rather than MAXPATHLEN, so this header only needs to resolve the
 * include and supply the couple of BSD-ism macros/aliases oksh's helpers reach for.
 */
#ifndef _MAIZE_SYS_PARAM_H_
#define _MAIZE_SYS_PARAM_H_

#include <limits.h>

#ifndef MAXPATHLEN
#define MAXPATHLEN PATH_MAX
#endif

#ifndef howmany
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#endif /* _MAIZE_SYS_PARAM_H_ */
