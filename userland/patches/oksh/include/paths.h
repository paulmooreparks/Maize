/*
 * paths.h (maize-94): Maize-local shim. oksh's main.c/exec.c/confstr.c include
 * <paths.h> for _PATH_BSHELL / _PATH_DEFPATH / _PATH_STDPATH. oksh's own portable.h
 * already provides #ifndef-guarded fallbacks for all three (portable.h:54-64), so
 * this header only needs to resolve the include under -nostdinc. Left intentionally
 * empty; portable.h supplies the macros.
 */
#ifndef _MAIZE_PATHS_H_
#define _MAIZE_PATHS_H_
#endif /* _MAIZE_PATHS_H_ */
