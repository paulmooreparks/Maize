/* maize-278 cycle-1 fix-pass regression fixture (review #3052 finding 1).
 *
 * A non-RT sibling header living beside the source that includes it, "quote"
 * included so the search is #include "sibling.h", not <sibling.h>: it must
 * resolve via the source's OWN directory (-I <source-dir>, added AFTER
 * toolchain/rt so RT headers keep priority), not via toolchain/rt. */
#ifndef RELPATH_SIBLING_H
#define RELPATH_SIBLING_H

#define RELPATH_SIBLING_EXIT_CODE 55

#endif
