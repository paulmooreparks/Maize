/* toolchain/rt/math.h -- freestanding <math.h> slice for the Maize C runtime
 * (maize-147).
 *
 * DOOM's v_video.c uses fabs live; the r_main trig (sin/cos/tan/atan2) is dead
 * `#if 0` in the DOOM tree and MUST NOT be declared here. This header DECLARES only
 * fabs; the body lives in the sibling libc card (maize-148), which implements it via
 * a sign-bit mask (irrelevant to the declaration). Do not over-scope: adding trig
 * decls here would invite unresolved symbols with no live caller.
 */
#ifndef MAIZE_MATH_H
#define MAIZE_MATH_H

double fabs(double x);

#endif /* MAIZE_MATH_H */
