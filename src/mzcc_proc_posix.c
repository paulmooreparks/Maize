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
#include <fcntl.h>
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
    /* Self-pipe (review #3052 finding 3): fork()+exec() splits "spawn" across
       two OS calls, so a plain execvp/execvp failure happens in the CHILD,
       invisible to the parent's return value; the documented "0 = spawn
       failure" contract (mzcc_proc.h) was only honored on Win32, where
       CreateProcessA's own return code covers it. The child writes its errno
       here and exits 127 on a pre-exec chdir/exec failure; FD_CLOEXEC on both
       ends means a SUCCESSFUL exec closes the write end for free, so the
       parent's read() returns 0 (EOF) with nothing written. Either way the
       read returns promptly: it never blocks on stdout/stderr traffic. */
    int spawn_err_pipe[2] = { -1, -1 };
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0 ||
        pipe(spawn_err_pipe) != 0) {
        close_fd(&in_pipe[0]);  close_fd(&in_pipe[1]);
        close_fd(&out_pipe[0]); close_fd(&out_pipe[1]);
        close_fd(&err_pipe[0]); close_fd(&err_pipe[1]);
        close_fd(&spawn_err_pipe[0]); close_fd(&spawn_err_pipe[1]);
        return 0;
    }
    fcntl(spawn_err_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(spawn_err_pipe[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        close_fd(&in_pipe[0]);  close_fd(&in_pipe[1]);
        close_fd(&out_pipe[0]); close_fd(&out_pipe[1]);
        close_fd(&err_pipe[0]); close_fd(&err_pipe[1]);
        close_fd(&spawn_err_pipe[0]); close_fd(&spawn_err_pipe[1]);
        return 0;
    }
    if (pid == 0) {
        /* Child: wire the pipe ends onto stdin/stdout/stderr, then exec. */
        close(spawn_err_pipe[0]);
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        dup2(err_pipe[1], 2);
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        if (cwd && cwd[0]) {
            if (chdir(cwd) != 0) {
                int e = errno;
                ssize_t wr = write(spawn_err_pipe[1], &e, sizeof(e));
                (void)wr;
                _exit(127);
            }
        }
        if (strchr(exe, '/')) {
            execv(exe, argv);
        } else {
            execvp(exe, argv);
        }
        {
            int e = errno;
            ssize_t wr = write(spawn_err_pipe[1], &e, sizeof(e));
            (void)wr;
        }
        _exit(127); /* exec failed */
    }

    /* Parent. */
    close_fd(&in_pipe[0]);
    close_fd(&out_pipe[1]);
    close_fd(&err_pipe[1]);
    close_fd(&spawn_err_pipe[1]);

    int spawn_errno = 0;
    ssize_t spawn_err_n = read(spawn_err_pipe[0], &spawn_errno, sizeof(spawn_errno));
    close_fd(&spawn_err_pipe[0]);
    if (spawn_err_n > 0) {
        /* Pre-exec failure: reap the zombie, tear down the still-open ends,
           and report spawn failure per the documented contract. */
        close_fd(&in_pipe[1]);
        close_fd(&out_pipe[0]);
        close_fd(&err_pipe[0]);
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            /* retry */
        }
        return 0;
    }

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
    /* Same self-pipe contract fix as run_proc (review #3052 finding 3): this
       primitive documents "0 on spawn failure" too (mzcc_proc.h), and
       run_inherit's own callers (--build, -r) need it just as much. */
    int spawn_err_pipe[2] = { -1, -1 };
    if (pipe(spawn_err_pipe) != 0) {
        return 0;
    }
    fcntl(spawn_err_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(spawn_err_pipe[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        close_fd(&spawn_err_pipe[0]);
        close_fd(&spawn_err_pipe[1]);
        return 0;
    }
    if (pid == 0) {
        close(spawn_err_pipe[0]);
        if (strchr(exe, '/')) {
            execv(exe, argv);
        } else {
            execvp(exe, argv);
        }
        {
            int e = errno;
            ssize_t wr = write(spawn_err_pipe[1], &e, sizeof(e));
            (void)wr;
        }
        _exit(127);
    }
    close_fd(&spawn_err_pipe[1]);
    int spawn_errno = 0;
    ssize_t spawn_err_n = read(spawn_err_pipe[0], &spawn_errno, sizeof(spawn_errno));
    close_fd(&spawn_err_pipe[0]);
    if (spawn_err_n > 0) {
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            /* retry */
        }
        return 0;
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

/* ---- portable thread / mutex / nproc primitives (maize-274) ------------- */

struct MzThread {
    pthread_t     tid;
    void       *(*fn)(void *);
    void         *arg;
};

struct MzMutex {
    pthread_mutex_t m;
};

static void *mz_thread_trampoline(void *p) {
    struct MzThread *t = (struct MzThread *)p;
    return t->fn(t->arg);
}

MzThread *mz_thread_start(void *(*fn)(void *), void *arg) {
    struct MzThread *t = (struct MzThread *)malloc(sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(&t->tid, NULL, mz_thread_trampoline, t) != 0) {
        free(t);
        return NULL;
    }
    return t;
}

void mz_thread_join(MzThread *t) {
    if (!t) {
        return;
    }
    pthread_join(t->tid, NULL);
    free(t);
}

MzMutex *mz_mutex_new(void) {
    struct MzMutex *m = (struct MzMutex *)malloc(sizeof(*m));
    if (!m) {
        return NULL;
    }
    if (pthread_mutex_init(&m->m, NULL) != 0) {
        free(m);
        return NULL;
    }
    return m;
}

void mz_mutex_lock(MzMutex *m)   { if (m) { pthread_mutex_lock(&m->m); } }
void mz_mutex_unlock(MzMutex *m) { if (m) { pthread_mutex_unlock(&m->m); } }

void mz_mutex_free(MzMutex *m) {
    if (m) {
        pthread_mutex_destroy(&m->m);
        free(m);
    }
}

int mzcc_nproc(void) {
#if defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int)n : 0;
#else
    return 0;
#endif
}

unsigned long mzcc_pid(void) {
    return (unsigned long)getpid();
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
