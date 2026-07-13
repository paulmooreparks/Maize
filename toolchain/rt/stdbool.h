/* toolchain/rt/stdbool.h -- freestanding <stdbool.h> for the Maize C runtime
 * (maize-146).
 *
 * cc-maize.sh preprocesses with `cpp -nostdinc -I toolchain/rt`, so no system
 * <stdbool.h> is visible; the DOOM tree (maize-145) uses bool/true/false. cproc
 * supports _Bool (type.c typebool, size 1), so this is the thin standard shim
 * mapping the macro names onto it. Header only, no runtime object.
 */
#ifndef MAIZE_STDBOOL_H
#define MAIZE_STDBOOL_H

#define bool  _Bool
#define true  1
#define false 0
#define __bool_true_false_are_defined 1

#endif /* MAIZE_STDBOOL_H */
