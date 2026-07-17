/*
 * sys/file.h (maize-94): Maize-local shim. oksh's history.c uses flock() to guard
 * the history file. quesOS is single-user with no advisory record locking, so the
 * backing flock() (maize_stubs.c) is a success no-op: history reads/writes are never
 * contended under wave-1's foreground-only, one-job-at-a-time model (decision 8947).
 * LOCK_EX / LOCK_UN come from oksh's portable.h (#ifndef-guarded); LOCK_SH is added
 * here for completeness.
 */
#ifndef _MAIZE_SYS_FILE_H_
#define _MAIZE_SYS_FILE_H_

#ifndef LOCK_SH
#define LOCK_SH 0x01
#endif
#ifndef LOCK_EX
#define LOCK_EX 0x02
#endif
#ifndef LOCK_NB
#define LOCK_NB 0x04
#endif
#ifndef LOCK_UN
#define LOCK_UN 0x08
#endif

int flock(int fd, int operation);

#endif /* _MAIZE_SYS_FILE_H_ */
