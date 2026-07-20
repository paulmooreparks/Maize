/* maize-278 cycle-1 fix-pass regression fixture (review #3052 finding 1):
 * a source that #include "sibling.h"'s a non-RT sibling header in its OWN
 * directory, compiled by a RELATIVE path from a DIFFERENT cwd than the
 * source's own directory.
 *
 * mzcc's compile_tu spawns cpp with cwd = the empty CPP_CWD scratch dir
 * (decision DI6) and passes "-I <source-dir>" as a discrete argv entry. When
 * <source-dir> is derived as a RELATIVE path (dir_of(src), unabsolutized), it
 * resolves against CPP_CWD, not against the caller's real location, so this
 * #include fails under mzcc while cc-maize.sh (which runs cpp in the
 * caller's real cwd) compiles it fine. See src/mzcc.c:abs_dir_of. */
#include "sibling.h"

int main(void) {
    return RELPATH_SIBLING_EXIT_CODE;
}
