/* mzcc_proc_win32.c (maize-278): Windows backend for the mzcc process-spawn
   abstraction. Guarded on _WIN32 so it compiles to nothing on POSIX, mirroring
   the presenter_transport_win32.cpp convention.

   The DI2 spawn seam in its Win32 form: CreatePipe x3 (child ends inheritable,
   parent ends made non-inheritable via SetHandleInformation), CreateProcessA
   with STARTF_USESTDHANDLES pointing at the child ends and lpCurrentDirectory =
   cwd (DI6), then a stdin-pump thread + a stderr-reader thread while the main
   thread drains stdout, then WaitForSingleObject + GetExitCodeProcess. No shell
   is involved: native paths are passed to native exes directly, so the whole
   cygpath/native_path marshalling layer cc-maize.sh needs does not exist here
   (design D6). */
#include "mzcc_proc.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Append one argv token to a command-line buffer using the CommandLineToArgvW
   quoting rules (quote when empty or containing space/tab/quote; double the
   backslashes that precede a quote or the closing quote). */
static void append_quoted(ByteBuf *cmd, const char *arg) {
    size_t i;
    int needs_quote = (arg[0] == '\0');
    for (i = 0; arg[i]; ++i) {
        char c = arg[i];
        if (c == ' ' || c == '\t' || c == '"') {
            needs_quote = 1;
            break;
        }
    }
    if (!needs_quote) {
        byte_buf_append(cmd, arg, strlen(arg));
        return;
    }
    byte_buf_append(cmd, "\"", 1);
    for (i = 0; arg[i];) {
        size_t nslash = 0;
        while (arg[i] == '\\') {
            ++nslash;
            ++i;
        }
        if (arg[i] == '\0') {
            /* Escape all trailing backslashes so they do not eat the close quote. */
            size_t k;
            for (k = 0; k < nslash * 2; ++k) {
                byte_buf_append(cmd, "\\", 1);
            }
            break;
        } else if (arg[i] == '"') {
            size_t k;
            for (k = 0; k < nslash * 2 + 1; ++k) {
                byte_buf_append(cmd, "\\", 1);
            }
            byte_buf_append(cmd, "\"", 1);
            ++i;
        } else {
            size_t k;
            for (k = 0; k < nslash; ++k) {
                byte_buf_append(cmd, "\\", 1);
            }
            byte_buf_append(cmd, &arg[i], 1);
            ++i;
        }
    }
    byte_buf_append(cmd, "\"", 1);
}

static char *build_command_line(char *const *argv) {
    ByteBuf cmd;
    byte_buf_init(&cmd);
    int i;
    for (i = 0; argv[i]; ++i) {
        if (i > 0) {
            byte_buf_append(&cmd, " ", 1);
        }
        append_quoted(&cmd, argv[i]);
    }
    byte_buf_append(&cmd, "\0", 1); /* NUL-terminate */
    return cmd.data; /* caller frees */
}

typedef struct {
    HANDLE      h;
    const char *data;
    size_t      len;
} stdin_pump_arg;

static DWORD WINAPI stdin_pump(LPVOID p) {
    stdin_pump_arg *a = (stdin_pump_arg *)p;
    size_t off = 0;
    while (off < a->len) {
        DWORD wrote = 0;
        DWORD chunk = (DWORD)((a->len - off > 0x10000) ? 0x10000 : (a->len - off));
        if (!WriteFile(a->h, a->data + off, chunk, &wrote, NULL)) {
            break; /* child closed its stdin early; stop. */
        }
        off += wrote;
    }
    CloseHandle(a->h);
    return 0;
}

typedef struct {
    HANDLE   h;
    ByteBuf *buf;
} reader_arg;

static DWORD WINAPI reader_thread(LPVOID p) {
    reader_arg *a = (reader_arg *)p;
    char tmp[65536];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(a->h, tmp, sizeof(tmp), &got, NULL)) {
            break; /* pipe closed (child exited): ERROR_BROKEN_PIPE => EOF. */
        }
        if (got == 0) {
            break;
        }
        if (byte_buf_append(a->buf, tmp, (size_t)got) != 0) {
            break;
        }
    }
    return 0;
}

int run_proc(const char *exe, char *const *argv, int argc,
             const char *stdin_bytes, size_t stdin_len, const char *cwd,
             ProcResult *out) {
    (void)argc;
    (void)exe;
    byte_buf_init(&out->stdout_bytes);
    byte_buf_init(&out->stderr_bytes);
    out->exit_code = -1;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE in_r = NULL, in_w = NULL;
    HANDLE out_r = NULL, out_w = NULL;
    HANDLE err_r = NULL, err_w = NULL;

    if (!CreatePipe(&in_r, &in_w, &sa, 0) ||
        !CreatePipe(&out_r, &out_w, &sa, 0) ||
        !CreatePipe(&err_r, &err_w, &sa, 0)) {
        goto fail;
    }
    /* The parent-retained ends must NOT be inherited by the child, else the
       child holds them open and the reader never sees EOF. */
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

    char *cmdline = build_command_line(argv);
    if (!cmdline) {
        goto fail;
    }

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = err_w;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    BOOL ok = CreateProcessA(
        NULL, cmdline, NULL, NULL, TRUE, 0, NULL,
        (cwd && cwd[0]) ? cwd : NULL, &si, &pi);
    free(cmdline);
    if (!ok) {
        goto fail;
    }
    CloseHandle(pi.hThread);

    /* Close the child ends in the parent; only the child holds them now. */
    CloseHandle(in_r);  in_r = NULL;
    CloseHandle(out_w); out_w = NULL;
    CloseHandle(err_w); err_w = NULL;

    stdin_pump_arg spa;
    spa.h = in_w;
    spa.data = stdin_bytes;
    spa.len = stdin_len;
    HANDLE stdin_th = CreateThread(NULL, 0, stdin_pump, &spa, 0, NULL);
    if (!stdin_th) {
        stdin_pump(&spa);
        in_w = NULL;
    }

    reader_arg era;
    era.h = err_r;
    era.buf = &out->stderr_bytes;
    HANDLE err_th = CreateThread(NULL, 0, reader_thread, &era, 0, NULL);

    reader_arg oa;
    oa.h = out_r;
    oa.buf = &out->stdout_bytes;
    reader_thread(&oa);

    if (err_th) {
        WaitForSingleObject(err_th, INFINITE);
        CloseHandle(err_th);
    } else {
        reader_thread(&era);
    }
    if (stdin_th) {
        WaitForSingleObject(stdin_th, INFINITE);
        CloseHandle(stdin_th);
    }

    CloseHandle(out_r); out_r = NULL;
    CloseHandle(err_r); err_r = NULL;

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    out->exit_code = (int)code;
    return 1;

fail:
    if (in_r)  CloseHandle(in_r);
    if (in_w)  CloseHandle(in_w);
    if (out_r) CloseHandle(out_r);
    if (out_w) CloseHandle(out_w);
    if (err_r) CloseHandle(err_r);
    if (err_w) CloseHandle(err_w);
    return 0;
}

int run_inherit(const char *exe, char *const *argv, int argc, int *exit_code) {
    (void)argc;
    (void)exe;
    char *cmdline = build_command_line(argv);
    if (!cmdline) {
        return 0;
    }
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    free(cmdline);
    if (!ok) {
        return 0;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (exit_code) {
        *exit_code = (int)code;
    }
    return 1;
}

int mzcc_self_path(const char *argv0, char *out, size_t cap) {
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (n > 0 && n < cap) {
        return 0;
    }
    if (argv0 && argv0[0]) {
        size_t k = strlen(argv0);
        if (k < cap) {
            memcpy(out, argv0, k + 1);
            return 0;
        }
    }
    return -1;
}

int mzcc_make_temp_dir(char *out, size_t cap) {
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(sizeof(tmp), tmp);
    if (n == 0 || n >= sizeof(tmp)) {
        return -1;
    }
    /* GetTempPathA yields a trailing backslash. Try unique names until mkdir
       succeeds (a fresh directory nobody else holds). */
    DWORD pid = GetCurrentProcessId();
    unsigned attempt;
    for (attempt = 0; attempt < 4096; ++attempt) {
        DWORD tick = GetTickCount();
        int w = snprintf(out, cap, "%smzcc.%lu.%lu.%u",
                         tmp, (unsigned long)pid, (unsigned long)tick, attempt);
        if (w < 0 || (size_t)w >= cap) {
            return -1;
        }
        if (CreateDirectoryA(out, NULL)) {
            return 0;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            return -1;
        }
    }
    return -1;
}

/* ---- portable thread / mutex / nproc primitives (maize-274) ------------- */

struct MzThread {
    HANDLE        h;
    void       *(*fn)(void *);
    void         *arg;
    void         *ret;
};

struct MzMutex {
    CRITICAL_SECTION cs;
};

static DWORD WINAPI mz_thread_trampoline(LPVOID p) {
    struct MzThread *t = (struct MzThread *)p;
    t->ret = t->fn(t->arg);
    return 0;
}

MzThread *mz_thread_start(void *(*fn)(void *), void *arg) {
    struct MzThread *t = (struct MzThread *)malloc(sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->fn = fn;
    t->arg = arg;
    t->ret = NULL;
    t->h = CreateThread(NULL, 0, mz_thread_trampoline, t, 0, NULL);
    if (!t->h) {
        free(t);
        return NULL;
    }
    return t;
}

void mz_thread_join(MzThread *t) {
    if (!t) {
        return;
    }
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

MzMutex *mz_mutex_new(void) {
    struct MzMutex *m = (struct MzMutex *)malloc(sizeof(*m));
    if (!m) {
        return NULL;
    }
    InitializeCriticalSection(&m->cs);
    return m;
}

void mz_mutex_lock(MzMutex *m)   { if (m) { EnterCriticalSection(&m->cs); } }
void mz_mutex_unlock(MzMutex *m) { if (m) { LeaveCriticalSection(&m->cs); } }

void mz_mutex_free(MzMutex *m) {
    if (m) {
        DeleteCriticalSection(&m->cs);
        free(m);
    }
}

int mzcc_nproc(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (si.dwNumberOfProcessors > 0) ? (int)si.dwNumberOfProcessors : 0;
}

unsigned long mzcc_pid(void) {
    return (unsigned long)GetCurrentProcessId();
}

void mzcc_remove_tree(const char *path) {
    char pattern[MAX_PATH * 2];
    int w = snprintf(pattern, sizeof(pattern), "%s\\*", path);
    if (w < 0 || (size_t)w >= sizeof(pattern)) {
        return;
    }
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
                continue;
            }
            char child[MAX_PATH * 2];
            int cw = snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName);
            if (cw < 0 || (size_t)cw >= sizeof(child)) {
                continue;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                mzcc_remove_tree(child);
            } else {
                DeleteFileA(child);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(path);
}

#endif /* _WIN32 */
