/* mzcc_sched.c (maize-274): the platform-independent work-queue behind
   mzcc_run_jobs (spec section 3). Reuses the mz_thread / mz_mutex backend
   primitives; no OS #ifdef lives here. */
#include "mzcc_sched.h"

#include "mzcc_internal.h" /* xmalloc, die */
#include "mzcc_proc.h"     /* MzThread, MzMutex */

#include <stdlib.h>

typedef struct {
    MzJobFn  fn;
    void    *ctx;
    int      njobs;
    int      next;    /* shared next-index to dispatch (guarded by mtx) */
    int      failed;  /* set once any job returned nonzero (guarded by mtx) */
    MzMutex *mtx;
} SchedState;

static void *worker_main(void *p) {
    SchedState *s = (SchedState *)p;
    for (;;) {
        mz_mutex_lock(s->mtx);
        if (s->failed || s->next >= s->njobs) {
            mz_mutex_unlock(s->mtx);
            break;
        }
        int idx = s->next++;
        mz_mutex_unlock(s->mtx);

        int rc = s->fn(s->ctx, idx);
        if (rc != 0) {
            mz_mutex_lock(s->mtx);
            s->failed = 1;
            mz_mutex_unlock(s->mtx);
        }
    }
    return NULL;
}

int mzcc_run_jobs(MzJobFn fn, void *ctx, int njobs, int cap) {
    if (njobs <= 0) {
        return 0;
    }
    if (cap < 1) {
        cap = 1;
    }
    if (cap > njobs) {
        cap = njobs;
    }

    /* Serial reference (MAIZE_JOBS=1): run inline in index order, stop
       dispatching after the first failure. No thread is spawned. */
    if (cap == 1) {
        for (int i = 0; i < njobs; ++i) {
            if (fn(ctx, i) != 0) {
                return 1;
            }
        }
        return 0;
    }

    SchedState s;
    s.fn = fn;
    s.ctx = ctx;
    s.njobs = njobs;
    s.next = 0;
    s.failed = 0;
    s.mtx = mz_mutex_new();
    if (!s.mtx) {
        /* No mutex: degrade to the serial path rather than fail the build. */
        for (int i = 0; i < njobs; ++i) {
            if (fn(ctx, i) != 0) {
                return 1;
            }
        }
        return 0;
    }

    MzThread **threads = (MzThread **)xmalloc((size_t)cap * sizeof(MzThread *));
    int started = 0;
    for (int i = 0; i < cap; ++i) {
        threads[i] = mz_thread_start(worker_main, &s);
        if (threads[i]) {
            ++started;
        }
    }
    /* If not a single worker started, run the pool loop on this thread so the
       work still completes (thread creation failure must not wedge the build). */
    if (started == 0) {
        worker_main(&s);
    }
    for (int i = 0; i < cap; ++i) {
        if (threads[i]) {
            mz_thread_join(threads[i]);
        }
    }
    free(threads);

    int failed = s.failed;
    mz_mutex_free(s.mtx);
    return failed ? 1 : 0;
}
