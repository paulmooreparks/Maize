/* mzcc_proc_posix.c (maize-278): POSIX backend for the mzcc process-spawn
   abstraction. Guarded on !defined(_WIN32) so it compiles to nothing on
   Windows, mirroring the presenter_transport_posix.cpp convention.

   This is the DI2 spawn seam in its POSIX form: a forked child dup2's the pipe
   ends onto fds 0/1/2, optionally chdir's into the empty-scratch cwd (DI6), and
   execs the tool; the parent pumps stdin from a dedicated pthread while the main
   thread drains stdout and a second pthread drains stderr, then waitpid +
   WEXITSTATUS. fork/exec (rather than posix_spawn) is used because the cpp stage
   needs a pre-exec chdir into an empty directory, which posix_spawn expresses
   only through the non-portable addchdir_np extension; the abstract shape
   (spawn a piped/cwd-controlled child, capture its streams, propagate its exit
   status) is identical either way and is entirely internal to this TU. exec is
   resolved via execvp when `exe` has no slash (a bare `cc`/`gcc` off PATH,
   decision D4) and execv otherwise, mirroring presenter_transport_posix's
   posix_spawn/posix_spawnp split. */
#include "mzcc_proc.h"

#if !defined(_WIN32)

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    int         fd;
    const char *data;
    size_t      len;
} stdin_pump_arg;

/* Write all of stdin then close, on its own thread so a large feed cannot
   deadlock against the main thread draining stdout. */
static void *stdin_pump(void *p) {
    stdin_pump_arg *a = (stdin_pump_arg *)p;
    size_t off = 0;
    while (off < a->len) {
        ssize_t w = write(a->fd, a->data + off, a->len - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            break; /* child closed its stdin early (e.g. it errored); stop. */
        }
        off += (size_t)w;
    }
    close(a->fd);
    return NULL;
}

typedef struct {
    int      fd;
    ByteBuf *buf;
} reader_arg;

static void *reader_thread(void *p) {
    reader_arg *a = (reader_arg *)p;
    char tmp[65536];
    for (;;) {
        ssize_t r = read(a->fd, tmp, sizeof(tmp));
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (r == 0) {
            break;
        }
        if (byte_buf_append(a->buf, tmp, (size_t)r) != 0) {
            break;
        }
    }
    return NULL;
}

static void close_fd(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

int run_proc(const char *exe, char *const *argv, int argc,
             const char *stdin_bytes, size_t stdin_len, const char *cwd,
             ProcResult *out) {
    (void)argc;
    byte_buf_init(&out->stdout_bytes);
    byte_buf_init(&out->stderr_bytes);
    out->exit_code = -1;

    int in_pipe[2]  = { -1, -1 };
    int out_pipe[2] = { -1, -1 };
    int err_pipe[2] = { -1, -1 };
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        close_fd(&in_pipe[0]);  close_fd(&in_pipe[1]);
        close_fd(&out_pipe[0]); close_fd(&out_pipe[1]);
        close_fd(&err_pipe[0]); close_fd(&err_pipe[1]);
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close_fd(&in_pipe[0]);  close_fd(&in_pipe[1]);
        close_fd(&out_pipe[0]); close_fd(&out_pipe[1]);
        close_fd(&err_pipe[0]); close_fd(&err_pipe[1]);
        return 0;
    }
    if (pid == 0) {
        /* Child: wire the pipe ends onto stdin/stdout/stderr, then exec. */
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        dup2(err_pipe[1], 2);
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        if (cwd && cwd[0]) {
            if (chdir(cwd) != 0) {
                _exit(127);
            }
        }
        if (strchr(exe, '/')) {
            execv(exe, argv);
        } else {
            execvp(exe, argv);
        }
        _exit(127); /* exec failed */
    }

    /* Parent. */
    close_fd(&in_pipe[0]);
    close_fd(&out_pipe[1]);
    close_fd(&err_pipe[1]);

    stdin_pump_arg spa;
    spa.fd = in_pipe[1];
    spa.data = stdin_bytes;
    spa.len = stdin_len;
    pthread_t stdin_tid;
    int have_stdin_thread = (pthread_create(&stdin_tid, NULL, stdin_pump, &spa) == 0);
    if (!have_stdin_thread) {
        /* Fallback: pump inline (a very short feed cannot deadlock; a long one
           is unlikely to reach here as thread creation rarely fails). */
        stdin_pump(&spa);
        in_pipe[1] = -1;
    }

    reader_arg era;
    era.fd = err_pipe[0];
    era.buf = &out->stderr_bytes;
    pthread_t err_tid;
    int have_err_thread = (pthread_create(&err_tid, NULL, reader_thread, &era) == 0);

    /* Main thread drains stdout. */
    reader_arg oa;
    oa.fd = out_pipe[0];
    oa.buf = &out->stdout_bytes;
    reader_thread(&oa);

    if (have_err_thread) {
        pthread_join(err_tid, NULL);
    } else {
        reader_thread(&era);
    }
    if (have_stdin_thread) {
        pthread_join(stdin_tid, NULL);
    }

    close_fd(&out_pipe[0]);
    close_fd(&err_pipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        /* retry */
    }
    if (WIFEXITED(status)) {
        out->exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        out->exit_code = 128 + WTERMSIG(status);
    } else {
        out->exit_code = -1;
    }
    return 1;
}

int run_inherit(const char *exe, char *const *argv, int argc, int *exit_code) {
    (void)argc;
    pid_t pid = fork();
    if (pid < 0) {
        return 0;
    }
    if (pid == 0) {
        if (strchr(exe, '/')) {
            execv(exe, argv);
        } else {
            execvp(exe, argv);
        }
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        /* retry */
    }
    if (exit_code) {
        if (WIFEXITED(status)) {
            *exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            *exit_code = 128 + WTERMSIG(status);
        } else {
            *exit_code = -1;
        }
    }
    return 1;
}

int mzcc_self_path(const char *argv0, char *out, size_t cap) {
#if defined(__APPLE__)
    /* macOS: _NSGetExecutablePath fills a possibly-non-canonical path; good
       enough for the two-levels-up REPO_ROOT derivation. */
    extern int _NSGetExecutablePath(char *buf, unsigned int *bufsize);
    unsigned int sz = (unsigned int)cap;
    if (_NSGetExecutablePath(out, &sz) == 0) {
        return 0;
    }
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n > 0) {
        out[n] = '\0';
        return 0;
    }
#endif
    if (argv0 && argv0[0]) {
        size_t n = strlen(argv0);
        if (n < cap) {
            memcpy(out, argv0, n + 1);
            return 0;
        }
    }
    return -1;
}

int mzcc_make_temp_dir(char *out, size_t cap) {
    const char *base = getenv("TMPDIR");
    if (!base || !base[0]) {
        base = "/tmp";
    }
    char tmpl[4096];
    int n = snprintf(tmpl, sizeof(tmpl), "%s/mzcc.XXXXXX", base);
    if (n < 0 || (size_t)n >= sizeof(tmpl) || (size_t)n >= cap) {
        return -1;
    }
    char *res = mkdtemp(tmpl);
    if (!res) {
        return -1;
    }
    memcpy(out, res, strlen(res) + 1);
    return 0;
}

void mzcc_remove_tree(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                continue;
            }
            char child[4096];
            int n = snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            if (n > 0 && (size_t)n < sizeof(child)) {
                mzcc_remove_tree(child);
            }
        }
        closedir(d);
        rmdir(path);
    } else {
        remove(path);
    }
}

#endif /* !defined(_WIN32) */
