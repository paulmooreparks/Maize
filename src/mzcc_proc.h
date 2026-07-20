/* mzcc_proc.h (maize-278): the process-spawn / pipe abstraction the mzcc driver
   uses for every compiled toolchain stage (cpp, cproc-qbe, qbe, mazm, mzld,
   maize). ONE primitive, run_proc(), spawns an executable with a fully-captured
   stdin/stdout/stderr; run_inherit() spawns a child that shares the parent's
   stdio (the -r guest-run and the --build delegation).

   The interface is platform-independent; the backends live in
   mzcc_proc_posix.c (#if !defined(_WIN32)) and mzcc_proc_win32.c (#ifdef
   _WIN32), each internally guarded so the non-matching one compiles to nothing.
   This core/posix/win32 split mirrors the proven in-repo seam
   src/presenter_transport_{core,posix,win32}.cpp (decision DI2) and is the D7
   macOS "compile-target-away" shape, reproduced in C rather than C++.

   Every stage is invoked as an ABSTRACT tool (run_proc(exe, ...)), never a
   hard-coded exe literal, so maize-277's later "spawn `maize mazm.mzx` instead
   of mazm.exe" self-hosting substitution is a localized change at one call
   site. */
#ifndef MZCC_PROC_H
#define MZCC_PROC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Growable byte buffer: `len` bytes used of `cap` allocated. Used in place of
   C++'s std::string/std::vector for all in-flight byte data (the stdin feed,
   captured stdout/stderr, and the CRLF-strip / normalize buffers). */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} ByteBuf;

void byte_buf_init(ByteBuf *b);
/* Append n bytes; returns 0 on success, -1 on allocation failure. */
int  byte_buf_append(ByteBuf *b, const char *bytes, size_t n);
void byte_buf_free(ByteBuf *b);

typedef struct {
    int     exit_code;      /* child exit status (WEXITSTATUS / GetExitCodeProcess) */
    ByteBuf stdout_bytes;   /* fully-captured child stdout */
    ByteBuf stderr_bytes;   /* fully-captured child stderr */
} ProcResult;

/* Spawn `exe` with `argv` (argv[0..argc-1]; argv MUST be NULL-terminated at
   argv[argc]; argv[0] is the exe path as the child sees it). Write
   `stdin_bytes` (length `stdin_len`) to the child's stdin, then close it;
   capture the child's stdout and stderr fully into growable ByteBufs. `cwd`
   NULL or empty means inherit the parent's working directory; otherwise the
   child runs with its working directory set to `cwd` (the empty-scratch-dir
   base cpp needs, decision DI6).

   Returns 1 if the child ran (see out->exit_code), 0 on spawn failure (exe not
   found / OS error). On a 0 return out->exit_code is set to -1. The caller owns
   and frees out->stdout_bytes / out->stderr_bytes via byte_buf_free(). A large
   preprocessed stage output exceeds the OS pipe buffer, so stdin is pumped from
   a dedicated thread while the main thread drains stdout (and a second thread
   drains stderr), avoiding the write-all-then-read deadlock. */
int run_proc(const char *exe, char *const *argv, int argc,
             const char *stdin_bytes, size_t stdin_len, const char *cwd,
             ProcResult *out);

/* Spawn `exe` with `argv` sharing the parent's stdin/stdout/stderr (no pipes,
   no capture), wait, and report the child exit status via *exit_code. Used for
   the -r guest run (guest output must reach the terminal and propagate the
   guest exit code) and the --build delegation. Returns 1 if the child ran, 0 on
   spawn failure. */
int run_inherit(const char *exe, char *const *argv, int argc, int *exit_code);

/* Best-effort absolute path of the running mzcc executable, for REPO_ROOT
   discovery (the exe lives at REPO_ROOT/build/<preset>/mzcc). `argv0` is the
   fallback when the OS query is unavailable. Writes a NUL-terminated path into
   `out` (capacity `cap`); returns 0 on success, -1 on failure. */
int mzcc_self_path(const char *argv0, char *out, size_t cap);

/* Create a fresh, unique temporary directory (mktemp -d equivalent) under the
   system temp area, writing its NUL-terminated path into `out` (capacity
   `cap`). Returns 0 on success, -1 on failure. Platform-specific (GetTempPathA
   on Win32; $TMPDIR / /tmp on POSIX), so it lives in the backends. */
int mzcc_make_temp_dir(char *out, size_t cap);

/* Best-effort recursive delete of a directory tree (the scratch cleanup, the
   native analog of cc-maize.sh's `rm -rf "$WORK"`). Never fails loudly. */
void mzcc_remove_tree(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* MZCC_PROC_H */
