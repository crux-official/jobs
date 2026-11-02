// The JOB SYSTEM — a frame-scoped composition on the thread POOL (the
// standalone this package requires), which is a composition on the threading
// PRIMITIVE (the concern the pool requires). The logic (the frame-scoped
// pool, the safety-net barrier, the batch accounting) is platform-agnostic —
// the pool owns the workers, the queue, the barrier's wait strategy (block
// on native, spin on the web main thread), and the ceiling-keyed serial
// degradation. One source serves every platform.
//
// THE LAYER, precisely: the pool is a passive service (a Create/Destroy/
// Submit/WaitAll/WorkerCount surface with a pool handle, no frame concept).
// This package OWNS the pool for the life of the app (created once in
// Initialize, destroyed once in Shutdown) and adds the three frame facts:
//   1. the frame-scoped surface: the caller gets Push/WaitAll/WorkerCount/
//      LastBatchNS with NO pool handle (the pool is this package's state);
//   2. the safety-net barrier: this extension's Update (the engine walks the
//      extensions, THEN ticks the apps — engine.c) calls the pool's WaitAll
//      once per frame, so a batch pushed but not waited on is drained before
//      the next frame's apps tick (nothing accumulates across frames);
//   3. the batch accounting: LastBatchNS — the last completed batch's wall
//      time (the per-run performance fact for A/B comparisons).
//
// THE MODE (threaded vs serial) is NOT decided here: this package always
// creates the pool (auto count, the caller's allocator when one was set), and the pool resolves the
// mode off the primitive's concurrency FACT (the ceiling: the hardware's
// physical cores on native, the pre-run Web Worker pool size on an isolated
// web page, 0 for a threading_none composition or a non-isolated page). The
// composition is the mode — this package has no serial branch (the pool's
// proven serial mode, with its named degrade line, is the one). A
// ThreadPool_Create failure is FATAL, named: a cell that composed the job
// system and cannot honor it does not boot (no silent serial fallback — the
// mode is the composition, not a runtime decision).
//
// WHO MAY CALL: the app is the primary driver, but any extension may push
// jobs + wait from within its own main-thread calls (it `requires` this
// package — the include root, the link edge, and the composition check ride
// the requires line; the walk edge puts this package before the co-driver, so
// the pool exists before the co-driver's Initialize). A job NEVER calls the
// job system (a worker-side WaitAll is a deadlock hazard; the pure-
// computation contract is what keeps the web main-thread spin safe). See
// jobs_api.h for the full contract.
//
// STATE: a static record — one pool pointer, the batch accounting (a bool,
// two uint64_t), and the job-node allocator the app set (the TPAllocator by
// value + a bool; the pool captures it at create). This package allocates
// nothing itself — the job NODES the pool claims through that allocator are
// the pool's, released at the pool's barrier.

#include "crux_time.h" // the time concern's static rendering: Time_GetTicksNS (the batch timer)
#include "extension.h"
#include "jobs_api.h"
#include "kernel_types.h"
#include "logging.h"     // the mode line + the named fatal (this package's voice)
#include "thread_pool.h" // the POOL's static rendering: ThreadPool_Create/Destroy/Submit/WaitAll/WorkerCount

#include <stdbool.h>
#include <stdint.h>

// --- The state (a static record; the invariant: pool is non-NULL from
// Initialize's success to Shutdown — the engine's two-phase boot + reverse
// teardown guarantee no caller sees any other state) ---
static struct {
  CruxThreadPool *pool; // the frame-scoped pool (auto count, the mode is the pool's fact)
  bool in_batch;        // a batch is open (a Push since the last barrier return)
  uint64_t batch_start; // Time_GetTicksNS at the batch's first Push
  uint64_t last_batch_ns; // the last completed batch's wall time (LastBatchNS)
  TPAllocator alloc;    // the job-node owner (valid when has_alloc)
  bool has_alloc;       // SetAllocator ran before the pool's create
} g_jobs;

// --- The batch accounting. A BATCH is the span from its first Push to the
// next barrier return — whichever barrier closes it, the caller's own
// WaitAll or the per-frame safety net. Closing = wait, then record the time
// (the wait is the happens-before; the record is the fact). In serial the
// wait is a no-op (the jobs ran inline in their Pushes) and the record is the
// inline execution time — the serial half of the A/B number.

static void jobs_barrier(void) {
  ThreadPool_WaitAll(g_jobs.pool);
  if (g_jobs.in_batch) {
    g_jobs.last_batch_ns = Time_GetTicksNS() - g_jobs.batch_start;
    g_jobs.in_batch = false;
  }
}

// --- The served API (the static renderings' definitions) ---

void Jobs_Push(void (*func)(void *arg), void *arg) {
  if (!g_jobs.in_batch) {
    g_jobs.batch_start = Time_GetTicksNS();
    g_jobs.in_batch = true;
  }
  ThreadPool_Submit(g_jobs.pool, func, arg);
}

void Jobs_WaitAll(void) {
  jobs_barrier();
}

int Jobs_WorkerCount(void) {
  // 0 for a serial pool (the pool's WorkerCount reads 0 there, and for NULL).
  return ThreadPool_WorkerCount(g_jobs.pool);
}

uint64_t Jobs_LastBatchNS(void) {
  // 0 before the first batch completes (the initial state).
  return g_jobs.last_batch_ns;
}

void Jobs_SetMode(JobsMode mode) {
  // Pass-through (the surface's JobsMode is the pool's TPMode, 1:1). The mode
  // is a PER-BATCH fact at the pool: it names the batch the next WaitAll
  // opens, and the pool's barrier re-arms the default (lanes) after every
  // batch — so this is the caller's per-batch dial, not a mode that sticks.
  ThreadPool_SetMode(g_jobs.pool, (TPMode)mode);
}

void Jobs_SetAllocator(const TPAllocator *allocator) {
  // The pool's allocator is a CREATE-time fact (this package's Initialize
  // creates the pool), so the seam is set BEFORE the pool exists. The engine
  // order makes the app's Construct that moment (extensions' Construct, then
  // the apps' Construct, then the extensions' Initialize).
  if (g_jobs.pool != NULL) {
    LogError("[jobs] SetAllocator: the pool is already created (the allocator is a create-time "
             "fact) - the call is ignored");
    return;
  }
  g_jobs.has_alloc = (allocator != NULL);
  if (allocator != NULL) {
    // The pool's contract (thread_pool_types.h): alloc and reset are required;
    // free may be NULL (no per-node free - reset reclaims the nodes).
    if (allocator->alloc == NULL || allocator->reset == NULL) {
      LogError("[jobs] SetAllocator: the allocator is missing a required function (alloc and "
               "reset are required; free may be NULL) - the call is ignored");
      return;
    }
    g_jobs.alloc = *allocator;
  }
}

static JobsAPI g_jobs_api = {
    .version = {1, 0, 0},
    .Push = Jobs_Push,
    .WaitAll = Jobs_WaitAll,
    .WorkerCount = Jobs_WorkerCount,
    .LastBatchNS = Jobs_LastBatchNS,
    .SetAllocator = Jobs_SetAllocator,
    .SetMode = Jobs_SetMode,
};

void *Jobs_GetSpecificAPI(void) {
  return &g_jobs_api;
}

// --- The extension lifecycle ---

static bool jobs_construct(void) {
  return true; // a standalone: internal setup only, no concern bit to register
}

static bool jobs_initialize(void) {
  // THE MODE IS THE POOL'S FACT: auto (0) sizes to the hardware's physical
  // cores, clamped to the concurrency ceiling; a ceiling of 0 degrades the
  // pool named to serial (the pool's own line fires too — the same fact at
  // the two layers). No probe here: the composition is the mode. The
  // ALLOCATOR is the caller's fact (SetAllocator, the app's Construct): the
  // job-node owner — or the memory concern directly when none was set (the
  // general case).
  g_jobs.pool = g_jobs.has_alloc
                    ? ThreadPool_CreateWithAllocator(0, &g_jobs.alloc)
                    : ThreadPool_Create(0);
  if (g_jobs.pool == NULL) {
    // Allocation refused, or a refused worker: a cell that composed the job
    // system and cannot honor it does not boot — return false and the engine
    // aborts the walk and unwinds (no silent serial fallback).
    LogError("[jobs] Initialize: the pool refused to create (allocation or a refused worker) - "
             "a cell that composed the job system cannot honor it; refusing to boot (no silent "
             "serial fallback)");
    return false;
  }
  const int workers = ThreadPool_WorkerCount(g_jobs.pool);
  if (workers > 0) {
    Log("[jobs] the frame barrier is live: %d workers (the pool's auto count, clamped to the "
        "concurrency ceiling)",
        workers);
  } else {
    Log("[jobs] the frame barrier is live: serial (the pool runs inline; the mode is the "
        "composition)");
  }
  return true;
}

// The safety net: the engine walk calls this once per frame, BEFORE the apps'
// ticks (engine.c: the extension walk, then the host ticks the apps). A batch
// pushed but not waited on is drained before the next frame's apps tick.
// In serial this is a no-op wait that still closes + times the batch.
static void jobs_update(double deltaTime) {
  (void)deltaTime;
  jobs_barrier();
}

static void jobs_shutdown(void) {
  // The apps tear down before the extensions (engine order): no app ticks
  // after this. Close a still-open batch FIRST: the safety net drains one
  // frame late, and an app whose last tick pushed without waiting would
  // otherwise reach Destroy with jobs never run (and their nodes never freed
  // by a per-node-free allocator). No-op when the batch is empty.
  if (g_jobs.pool != NULL) {
    jobs_barrier();
  }
  // Destroy signals the workers, joins them, releases queued-but-unrun jobs,
  // reclaims the pool.
  ThreadPool_Destroy(g_jobs.pool);
  g_jobs.pool = NULL;
  g_jobs.in_batch = false;
}

// The extension interface. A standalone registers NO concern bit (it is not a
// capability — the threading concern is; this is a composition on it). Its
// GetSpecificAPI serves the JobsAPI the registry dispatches through
// PACKAGE_JOBS (the generated package_types.h).
ExtensionInterface g_extension_jobs = {
    "jobs",
    {1, 0, 0},
    jobs_construct,
    jobs_initialize,
    jobs_update,
    jobs_shutdown,
    NULL,
    Jobs_GetSpecificAPI,
};
