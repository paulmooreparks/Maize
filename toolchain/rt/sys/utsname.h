/* toolchain/rt/sys/utsname.h -- freestanding <sys/utsname.h> for the Maize C runtime
 * (maize-374).
 *
 * uname(2)'s public struct + prototype, matching the existing sys/stat.h / sys/wait.h /
 * sys/socket.h convention (a header declaring the type and prototype whose wrapper body
 * lives in a .c already in the RT build set -- here unistd.c, beside gethostname). It is
 * exactly what userland/sbase/uname.c includes, so that tool builds against it with no
 * change of its own.
 *
 * struct utsname mirrors the raw Linux kernel struct new_utsname exactly: six 65-byte
 * (__NEW_UTS_LEN 64 + NUL) NUL-terminated fields, 390 bytes total, so an unmodified musl-
 * or glibc-shaped uname() caller reads the fields at the offsets it expects with no shim.
 * The guest $3F handler (quesOS's do_uname) fills the image; the values are fixed
 * compile-time identity strings (sysname "quesOS", nodename "maize", machine "maizev1",
 * a quesOS release/version, empty domainname). uname() has no error path today: it
 * returns 0.
 */
#ifndef MAIZE_SYS_UTSNAME_H
#define MAIZE_SYS_UTSNAME_H

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

int uname(struct utsname *buf);

#endif /* MAIZE_SYS_UTSNAME_H */
