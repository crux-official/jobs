# jobs — the frame-scoped job system (a composition on `thread_pool`)

The `jobs` package is the **"give me the frame's job system"** layer: it owns one
worker pool for the life of the app, serves a four-call surface to the frame, and
calls the pool's barrier once per frame as a safety net. It is a `standalone`
extension (kind `standalone`, v1.0.0) built on the `thread_pool` standalone, which
is a composition on the `threading` concern — one source serves every platform.

The layering, bottom up: `threading` (the primitive: thread creation + the
concurrency facts) → `thread_pool` (the FIFO queue + batch barrier + auto-sizing,
platform-agnostic except the barrier's wait strategy) → **`jobs`** (the frame-scoped
pool + the safety net + the batch accounting) → the application (and any
co-driver, below).

## The API

The served surface (the `JobsAPI` struct in `include/jobs_api.h`; the static
rendering is plain externs in `include/static/jobs.h`):

| call | semantics |
|---|---|
| `Jobs_Push(fn, arg)` | enqueue a unit of work. Threaded: `fn` runs on a pool worker. Serial: `fn` runs **inline, immediately, on the calling thread** (the push is the execution). `arg` is caller-owned; it must outlive the batch's barrier. |
| `Jobs_WaitAll()` | the batch barrier: return when every pushed job has completed — the **happens-before**. A no-op wait when serial. |
| `Jobs_WorkerCount()` | the worker count the cell resolved to (the mode fact: 0 when serial). An app that REQUIRES threads checks this after boot and refuses on 0. |
| `Jobs_LastBatchNS()` | the last completed batch's wall time (ns): from the batch's first `Push` to the barrier that closed it. 0 before the first batch completes. |

The app never sees a pool handle, never creates anything, never checks "am I
threaded" — the pool is the package's private state.

## The job contract (what runs in a job)

A job is **pure computation into caller-owned memory**:

- it never touches the display/input/module surface, never the filesystem, never
  the module system;
- it never logs from a worker (logging is a main-thread service in the org's
  dialects);
- it never dispatches the engine (no `Engine_*` calls from a worker);
- it never proxies work back to the submitting (main, on web) thread;
- it **never calls the job system** (see the caller rules below).

Results ride in the caller's buffers; `Jobs_WaitAll` is the happens-before (on its
return, every job's writes are visible to the calling thread). Ordering among
parallel jobs is **unspecified** — a batch boundary (or the serial mode) is the
ordering. If two jobs touch the same memory, they race by construction: the 0037
probe's per-slot discipline (one job per slot, no shared fields) is the race-free
pattern.

## Who may drive it (the shared-service contract)

The job system is a **shared service, not an app-private one**. The application is
the primary driver, but **any extension may push jobs and wait on the barrier from
within its own main-thread calls** — a software renderer pushing its draw jobs
inside its `DrawSprites` call, a physics solver chunking its integration. The
wiring is one manifest line: the extension's manifest `requires` `jobs` (the
include root, the link edge, and the composition check all ride that line; the
walk edge puts `jobs` — and the pool — before the co-driver, so the pool exists
before the co-driver's `Initialize`). The dependency is a clean DAG
(`renderer → jobs → thread_pool → threading`); `jobs` knows nothing of renderers.

The four load-bearing caller rules:

1. **Co-drivers call from the main/app thread only.** The calls are sequential on
   the calling thread; each `WaitAll` returns before the next caller begins.
2. **A job never calls the job system.** A worker calling `WaitAll` is a
   **deadlock hazard** (it blocks a worker the batch still needs) — and it is
   exactly the "engine dispatch from a worker" the contract forbids. A worker
   calling `Push` is a scope escape: v1 has no fan-out model (an untracked spawn
   the current barrier's snapshot does not cover leaks to the next frame's drain),
   and a job system that lets jobs spawn jobs is a task graph wearing a trench
   coat — that is the v2 major, with its own scheduler state.
3. **The barrier is global (pool-level), not per-caller.** A `WaitAll` by any
   caller drains the pool's whole batch, not just that caller's pushes. Co-drivers
   that want their own work isolated **sequence their batches** (wait before the
   next caller pushes). Per-caller batch isolation is a clean v2 append-only
   growth.
4. **One shared pool is the design, not a compromise.** App + renderer + physics
   drawing from the *same* pool is the "data-splitting jobs, not subsystem
   threads" principle: per-subsystem pools idle the moment one subsystem's batch
   finishes before another's. v1 has no priorities — co-drivers' jobs interleave
   in the FIFO as equal-priority frame work.

A co-driver's own API is a separate design question: the job system only says the
co-driver's *implementation* may use the pool. (In the renderer case: the app
calls `DrawSprites(state)` synchronously; the renderer pushes its draw jobs and
waits — or pushes without waiting and lets the frame barrier drain them, a
"deferred render" — the renderer picks its own contract.)

## The mode: the composition is the mode

There is **no runtime switch** and no probe. `jobs`'s `Initialize` always creates
the pool (`ThreadPool_Create(0)`, auto), and the pool resolves the mode off the
`threading` primitive's **concurrency fact** (the ceiling):

| environment | ceiling | mode |
|---|---|---|
| native (linux/windows/macos), the platform's threading driver | the hardware's physical cores | threaded |
| web, a **cross-origin isolated** page (COOP + COEP headers) | `navigator.hardwareConcurrency` | threaded |
| web, a **plain** page (no COOP/COEP — no `SharedArrayBuffer`) | 0 | **serial, the named degrade** |
| a `threading_none` composition (any platform) | 0 | **serial, the named degrade** |

One source, every mode. A `ThreadPool_Create` failure (allocation refused, a
refused worker) is **fatal and named** — `Initialize` returns false and the engine
aborts the walk and unwinds: a cell that composed the job system and cannot honor
it does not boot. There is no silent serial fallback (the mode is the composition,
not a runtime decision).

The named lines to expect at boot: the pool's mode line (the threaded worker count,
or the `[thread_pool] ... the pool runs SERIAL` warning) and the jobs mode line
(`[jobs] the frame barrier is live: N workers ...` / `... serial ...`).

## The batch model (and `LastBatchNS`, honestly)

A **batch** is the span from its first `Push` after a barrier to the next barrier
return — whichever barrier closes it: the caller's own `WaitAll`, or the
**safety net** (`jobs`'s `Update`, which the engine walks *before* the apps' ticks
— so a batch pushed but not waited on is drained before the next frame's apps tick;
nothing accumulates across frames). `LastBatchNS` records that span's wall time.

The honest accounting: a waited batch is its own drain time; an **un-waited**
batch spans the frame gap and its recorded time is the drain time, *honestly*
(including the pacing). Serial batches are timed too (their inline execution
time). A per-run A/B harness that sums `LastBatchNS` after each barrier gets the
run's total time in the job system — one line per bounded run, the contract the
0040 A/B harness captures.

## Web deployment notes

- **Threaded web needs the two headers** (served on the page):
  `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: require-corp` (cross-origin isolation, which
  gates `SharedArrayBuffer`, which gates the emscripten pthread pool). The build
  is one `-pthread` binary; the page decides the mode at runtime.
- **A plain page is a first-class mode**, not a failure: no COOP/COEP → no SAB →
  ceiling 0 → the pool degrades **named** to serial (the pool's warning line + the
  jobs mode line fire). The same binary serves both pages.
- **Node needs no headers** (SAB is available there by default) — the node smoke
  runs threaded for that reason.
- The barrier's wait strategy is platform-owned by the pool: block on native,
  **spin** on the web main thread (the main thread cannot block its own frame
  pump). The job contract's "no engine dispatch from a worker" is what keeps the
  spin safe.

## v1/v2 line (what is deliberately NOT here)

- **Per-worker scratch** — v2. What is missing is the **worker identity** (the
  pool has no worker-index fact), not the arena: the arena concern
  (`ArenaCruxAPI`, v2-ready) is the backing. v1 scratch = the caller's own
  `arg` storage (the job contract's model).
- **Task graphs (jobs spawning jobs, dependencies)** — v2 major, with its own
  scheduler state. The caller rules above exist to keep v1's model honest.
- **Priorities / preemption** — v2 (the shared FIFO is the equal-priority frame
  work model; "render preempts sim" is the v2 feature).
- **Per-caller batch isolation** — v2 append-only growth (the barrier is
  pool-global in v1).
- **Explicit worker-count overrides** — not v1: the count is the pool's auto
  fact (physical cores, clamped to the ceiling).

## Wiring it (the build surface)

- The package: `jobs.extension` (`kind = "standalone"`, v1.0.0),
  `requires = [thread_pool 1.0.0, logging 1.1.0, time 1.1.0]`,
  `renderings = ["static", "dynamic"]`.
- A cell composes it like `thread_pool` does today — an `extensions` line next to
  the pool (the pool is mandatory through `jobs`' own `requires` anyway); the
  serial mode is a `threading = ...threading_none...` per-cell override, same
  source.
- The app (or co-driver) claims it by name in its `requires`
  (`{ package = "jobs", window = "1.0.0" }`) — the claim gate, the include
  grants, and the link edge all ride that line.
