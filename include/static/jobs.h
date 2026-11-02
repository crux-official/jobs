#ifndef CRUX_STATIC_JOBS_H
#define CRUX_STATIC_JOBS_H

#include <stdint.h>

// The job-node allocator type, OPAQUE (the pool's TPAllocator): the surface
// passes it by pointer only — a consumer needs no pool include (the complete
// type is for the code that constructs an allocator, which requires the pool).
struct TPAllocator;

// The batch dispatch mode, spelled at this layer (the same two facts as the
// pool's TPMode, 1:1 — the surface keeps its no-pool-include design):
//   JOBS_MODE_LANES — the default: even work, no stealing cost.
//   JOBS_MODE_STEAL — skewed batches: a worker that drains its own share
//     first pulls jobs off the busy workers' queues (measured ~8x recovery
//     on a lopsided batch where lanes stall near 1.1x).
// A per-batch fact: SetMode names it for the batch the NEXT barrier opens,
// and the default (LANES) holds again after every batch.
typedef enum JobsMode {
  JOBS_MODE_LANES = 0,
  JOBS_MODE_STEAL = 1,
} JobsMode;

#ifdef __cplusplus
extern "C" {
#endif

// The static rendering of the jobs contract: plain externs, defined by the
// package and linked into the binary (a static consumer pays zero
// indirection and cannot accidentally dispatch — the same shape as the other
// packages' static renderings). The API struct itself lives in jobs_api.h;
// this header never includes it (the externs need no package type — the
// frame-scoped surface takes no pool handle).
extern void Jobs_Push(void (*func)(void *arg), void *arg);
extern void Jobs_WaitAll(void);
extern int Jobs_WorkerCount(void);
extern uint64_t Jobs_LastBatchNS(void);
// The job-node allocator seam: the owner of the pool's job nodes (a hot
// arena child, a polyglot allocator over the ABI). Call it before the first
// batch (the app's Construct); no call = the memory concern directly (the
// general case).
extern void Jobs_SetAllocator(const struct TPAllocator *allocator);
// The batch dispatch mode (per-batch fact: the next barrier's batch; the
// default, lanes, holds again after every batch — see JobsMode above for
// when stealing pays).
extern void Jobs_SetMode(JobsMode mode);

#ifdef __cplusplus
}
#endif

#endif
