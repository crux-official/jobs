#ifndef CRUX_DYNAMIC_JOBS_H
#define CRUX_DYNAMIC_JOBS_H

#include "engine.h"
#include "jobs_api.h"
#include "package_types.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The dynamic rendering of the jobs contract: every call dispatches through
// the engine's registry channel — this package's served JobsAPI, selected by
// the package's generated dispatch id (PACKAGE_JOBS, the generated
// package_types.h). crux_engine_api is the module-global pointer defined by
// the install TU the DYNAMIC dialect compiles in and installed by the
// target's Construct (the same pattern as the other packages' dynamic
// renderings).
static inline void Jobs_Push(void (*func)(void *arg), void *arg) {
  ((JobsAPI *)crux_engine_api->GetExtensionAPI(PACKAGE_JOBS))->Push(func, arg);
}
static inline void Jobs_WaitAll(void) {
  ((JobsAPI *)crux_engine_api->GetExtensionAPI(PACKAGE_JOBS))->WaitAll();
}
static inline int Jobs_WorkerCount(void) {
  return ((JobsAPI *)crux_engine_api->GetExtensionAPI(PACKAGE_JOBS))->WorkerCount();
}
static inline uint64_t Jobs_LastBatchNS(void) {
  return ((JobsAPI *)crux_engine_api->GetExtensionAPI(PACKAGE_JOBS))->LastBatchNS();
}
// The job-node allocator seam (call before the first batch; no call = the
// memory concern directly).
static inline void Jobs_SetAllocator(const struct TPAllocator *allocator) {
  ((JobsAPI *)crux_engine_api->GetExtensionAPI(PACKAGE_JOBS))->SetAllocator(allocator);
}
// The batch dispatch mode (per-batch fact: the next barrier's batch; the
// default, lanes, holds again after every batch — see JobsMode in
// jobs_api.h for when stealing pays).
static inline void Jobs_SetMode(JobsMode mode) {
  ((JobsAPI *)crux_engine_api->GetExtensionAPI(PACKAGE_JOBS))->SetMode(mode);
}

#ifdef __cplusplus
}
#endif

#endif
