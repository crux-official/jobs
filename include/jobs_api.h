#ifndef CRUX_JOBS_API_H
#define CRUX_JOBS_API_H

#include "kernel_types.h"

#include <stdint.h>

// The job-node allocator type, OPAQUE (the pool's TPAllocator, defined in
// thread_pool_types.h): this surface only ever passes the allocator BY
// POINTER — a consumer of the frame-scoped surface needs no pool include at
// all. The complete type is for the code that CONSTRUCTS an allocator (the
// app that wires an arena child or a polyglot allocator — that code requires
// the pool).
struct TPAllocator;

// The batch dispatch mode, spelled at THIS layer (the same two facts as the
// pool's TPMode, 1:1 — the surface keeps its no-pool-include design, so the
// names are its own and the package maps them at the pool boundary):
//   JOBS_MODE_LANES — the default: even work, no stealing cost.
//   JOBS_MODE_STEAL — skewed batches: a worker that drains its own share
//     first pulls jobs off the busy workers' queues (measured ~8x recovery
//     on a lopsided batch where lanes stall near 1.1x); the tax is a
//     seq_cst fence per pop/steal, paid even when there is nothing to steal.
// A per-batch fact: SetMode names it for the batch the NEXT barrier opens,
// and the default (LANES) holds again after every batch.
typedef enum JobsMode {
  JOBS_MODE_LANES = 0,
  JOBS_MODE_STEAL = 1,
} JobsMode;

#ifdef __cplusplus
extern "C" {
#endif

// The jobs standalone's contract: the one struct of function pointers the
// package serves. This is the JOB SYSTEM layer — the frame-scoped pool + the
// safety-net barrier + the batch accounting, a composition on the thread_pool
// standalone (which is a composition on the threading primitive). The pool's
// operations (Create/Destroy/Submit/WaitAll/WorkerCount, with a pool handle)
// are NOT here: the caller gets the frame-scoped surface below, and the pool
// is this package's private state.
//
// Dispatch: through the engine's registry channel — this package's served
// JobsAPI, selected by the package's generated dispatch id (PACKAGE_JOBS, the
// generated package_types.h). The static rendering is the plain-externs
// dialect (zero indirection).
//
// HOW TO USE IT RIGHT (the whole discipline, in one place):
//
//   A BATCH is the span from its first Push to the next barrier return —
//   whichever barrier closes it: your own WaitAll, or the package's
//   per-frame safety net (it drains one batch late if you pushed and did not
//   wait). The intended frame shape:
//
//     Jobs_SetAllocator(&allocator);  // once, in the app's Construct (or
//                                     // never: the memory fallback)
//     for (each frame) {
//       ... optionally Jobs_SetMode(JOBS_MODE_STEAL);  // for THIS batch
//       Jobs_Push(fn, arg);        // push the frame's jobs (from the
//       Jobs_Push(fn, arg);        // main/app thread only)
//       Jobs_WaitAll();            // close the batch; use the results
//     }
//
//   1. PUSH FROM THE MAIN/APP THREAD ONLY, ONE AT A TIME. The push path is
//      lock-free, so two threads pushing to the same batch is a data race:
//      the pool records the first push's thread per batch and REFUSES every
//      push from a different thread NAMED (the job drops; the barrier still
//      closes). The submitter may change ACROSS batches, never within one.
//   2. A JOB NEVER CALLS THE JOB SYSTEM. A submitted job is pure computation
//      and must not Push or WaitAll: a worker calling WaitAll is a DEADLOCK
//      HAZARD (it blocks a worker the batch still needs).
//   3. A JOB's `arg` storage OUTLIVES THE BATCH (it must still be valid when
//      the worker runs the job, which can be after the Push returned).
//   4. THE BARRIER IS THE HAPPENS-BEFORE: on WaitAll's return, every job's
//      writes are visible to the calling thread. Ordering AMONG parallel jobs
//      is unspecified (a batch boundary is the ordering).
//   5. THE MODE is a per-batch fact (Jobs_SetMode, above): keep the default
//      (lanes) for even work; steer a known-skewed batch to stealing.
//   6. THE BATCH BOUND: 4096 jobs per slot (the pool's per-slot window); a
//      slot that outgrows it drops the excess NAMED (the barrier still
//      closes). Frame-sized batches are nowhere near it.
//
// WHO MAY CALL (the shared-service contract): the job system is a SHARED
// SERVICE, not an app-private one. The application is the primary driver, but
// ANY extension may call these from within its own MAIN-THREAD calls (a
// software renderer pushing its draw jobs inside its Draw call) — it declares
// the dependency by `requires`-ing this package in its manifest (the include
// root, the link edge, and the composition check all ride that line). The
// rules above are load-bearing for co-drivers too: main/app thread only,
// sequential batches.
// The barrier is GLOBAL (pool-level): a WaitAll by ANY caller drains the
// pool's whole batch, not just that caller's pushes. Callers that want their
// own work isolated sequence their batches (wait before the next caller
// pushes).
//
// The job contract (what runs in a job): a job is pure computation into
// caller-owned memory — it never touches the display/input/module surface,
// never the filesystem, never logs from a worker, never dispatches the
// engine, and never proxies work back to the submitting (main, on web)
// thread (the web barrier spins on the main thread and would deadlock on
// such a proxy). Results ride in the caller's buffers.

typedef struct JobsAPI {
  PackageVersion version;

  // Enqueue a unit of work. Threaded: fn runs on a pool worker. Serial: fn
  // runs INLINE, immediately, on the calling thread (the push is the
  // execution). arg is the caller's storage; it must outlive the batch's
  // barrier.
  void (*Push)(void (*func)(void *arg), void *arg);

  // The batch barrier: return when every pushed job has completed (the
  // happens-before). A no-op wait when serial. The jobs extension also calls
  // this once per frame as a safety net (its Update, the engine walk before
  // the apps' ticks) — a batch pushed but not waited on is drained before the
  // next frame's apps tick.
  void (*WaitAll)(void);

  // The worker count the cell resolved to (0 when serial — the mode fact).
  // An app that REQUIRES threads checks this after boot and refuses on 0
  // (the threading contract, unchanged one layer up).
  int (*WorkerCount)(void);

  // The last completed batch's wall time (ns): from the batch's first Push to
  // the barrier that closed it — the caller's own WaitAll, or the per-frame
  // safety net (an un-waited batch spans the frame gap, and its time is the
  // drain time, honestly). 0 before the first batch completes. The per-run
  // performance fact for A/B comparisons of the job system.
  uint64_t (*LastBatchNS)(void);

  // The job-NODE allocator seam, passed through to the pool: the caller hands
  // the job system the owner of its job nodes (the pool's TPAllocator —
  // thread_pool_types.h): a hot arena child (Claim / no-op free / barrier
  // Reset — the measured zero-malloc hot path, ~3.5x faster than the
  // fallback), or a polyglot caller's own allocator over the ABI. The pool's
  // allocator is a CREATE-time fact, so this must be called BEFORE the first
  // batch (the app's Construct is the intended moment — it runs before this
  // package's Initialize, where the pool is created). NO call — the general
  // case, the arena is an optional concern — and the pool uses the memory
  // concern directly.
  void (*SetAllocator)(const struct TPAllocator *allocator);

  // The batch dispatch mode (JobsMode), passed through to the pool: a
  // per-batch fact, effective for the batch the NEXT barrier opens; the
  // default (lanes) holds again after every batch. Use stealing for a batch
  // known to be skewed (a cluster of heavy jobs — the measured case: a
  // quarter of the slots carrying most of the frame's work); keep the
  // default otherwise (its no-steal hot path is the faster one on even work).
  void (*SetMode)(JobsMode mode);
} JobsAPI;

#ifdef __cplusplus
}
#endif

#endif
