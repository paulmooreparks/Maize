/* mzcc_sched.h (maize-274): the parallel TU work-queue (spec section 3). A
   fixed-size worker pool over a mutex-guarded shared next-index counter, built
   on the portable mz_thread / mz_mutex primitives (mzcc_proc.h). It is
   deliberately platform-INDEPENDENT and pipeline-agnostic: it runs an opaque
   per-index callback, so the compile-pipeline Job descriptors (result slot,
   diag sink, canonical link position) live in the caller (mzcc.c) and the
   scheduler only owns the dispatch discipline.

   Determinism is the caller's contract, not the scheduler's: the scheduler
   dispatches indices in increasing order but completes them in arbitrary order,
   so the caller must assemble its output (the mzld object vector) in canonical
   index order after this returns, never in completion order. */
#ifndef MZCC_SCHED_H
#define MZCC_SCHED_H

/* Per-index worker callback. Run job `index`; return 0 on success, nonzero on
   failure. Invoked concurrently for DISTINCT indices, so it must touch only
   index-private state (each job writes its own pre-assigned result/diag slot). */
typedef int (*MzJobFn)(void *ctx, int index);

/* Run jobs [0, njobs) via fn(ctx, i), using up to `cap` concurrent workers over
   a mutex-guarded shared next-index. Indices are DISPATCHED in increasing
   order. Once any job returns nonzero, no NEW job is dispatched (in-flight jobs
   still finish); because dispatch is monotonic, every index below the
   lowest-failing one has already been dispatched, so the caller can find the
   deterministic lowest-index failure by scanning its own job slots afterward.

   cap <= 1 runs every job inline on the calling thread in index order (the
   MAIZE_JOBS=1 serial reference path: no thread is spawned, so it is the
   cleanest possible determinism baseline). Returns 0 if every dispatched job
   succeeded, 1 if any failed. */
int mzcc_run_jobs(MzJobFn fn, void *ctx, int njobs, int cap);

#endif /* MZCC_SCHED_H */
