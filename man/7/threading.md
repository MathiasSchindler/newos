# THREADING

## NAME

threading - project-owned concurrency model, parallel execution, and platform plan

## DESCRIPTION

This is the project-owned concurrency design for the freestanding userland. It is not a pthread compatibility layer, not a C11 threads implementation, and not a thin wrapper that re-exports kernel threads to tool code. Because every caller lives in this repository and there are no external consumers, the design is free to pick the concurrency model that produces the smallest, fastest, most predictable binaries rather than the model history handed us.

The central decision of this design is that tools do not program against raw threads. They program against two high-level models — a parallel task pool and an I/O readiness loop — and the threads, syscalls, and wait/wake primitives sit underneath as a private substrate. This inversion is what lets the project keep the non-negotiable constraints while still being ambitious:

- a single-threaded tool pays exactly nothing: no extra code, no allocator locking, no TLS setup, no startup cost
- nothing here introduces an external dependency, a standard C library call, or `libSystem`/dylib imports on macOS
- the same tool source runs correctly on every platform, including platforms whose native thread backend does not exist yet, because serial execution is a real backend rather than an error path

This page describes that two-layer model, the public APIs tools actually call, the private substrate beneath them, and the current platform status for Linux, project-linked macOS, and hosted verification builds. It is also a migration guide: a developer or LLM implementing a threaded tool should be able to choose the right model, keep the serial path correct, preserve deterministic output, and know what to measure before calling the work complete.

## THE TWO AXES OF CONCURRENCY

Almost every "this tool needs concurrency" request in this repository is really one of two distinct problems, and conflating them produces code that is both slower and larger than necessary.

CPU parallelism is the need to keep several cores busy on work that is compute-bound and splittable: compression and archive blocks, image transforms, compiler translation units, linker sections, SQL scan and sort phases, hashing and checksum sweeps. The work is divisible into independent chunks, the chunks rarely touch shared mutable state, and the goal is throughput proportional to core count. The right tool is a worker pool plus a fork/join API.

I/O concurrency is the need to make progress on many connections or files that spend their time waiting, not computing: downloaders fetching several files, `httpd` and `sshd` serving multiple clients, future service-style tools. The bottleneck is the network or disk, not the CPU. Spending a kernel thread and a half-megabyte stack per connection — and dragging the global allocator into a locked mode to do it — is the wrong trade. The right tool is a single-threaded readiness loop over `epoll`/`kqueue`/`io_uring` with no shared-state locking at all.

Threads are used only where they earn their cost: CPU parallelism. I/O concurrency is handled by an I/O readiness loop. The two models are orthogonal and may be combined — for example, an I/O-loop server that hands a CPU-heavy request off to the task pool — but they are designed and reasoned about separately.

## NAMING AND API OWNERSHIP

The public names should make it obvious that this is the project's own concurrency model, not a pthread, C11, OpenMP, TBB, libuv, or kernel API in disguise. Names are therefore prefixed by the layer that owns the contract.

Layer 1 model APIs use the runtime prefix `rt_` and project-owned type names such as `RtTaskPool`, `RtTaskGroup`, and `RtIoLoop`. The names describe the model the tool is using, not the mechanism below it. A tool calls `rt_parallel_for` or `rt_io_loop_run`; it does not call a symbol named after a kernel object, a pthread concept, or a generic third-party convention that already carries expectations from another ecosystem.

Layer 0 substrate APIs use the `platform_` prefix and neutral operation names. The wait/wake primitive is named as waiting on a 32-bit word, not as `futex`, because futex is only the Linux implementation. Linux, Darwin, and future platforms can provide very different syscalls under the same `platform_wait_word` contract without leaking their vocabulary into shared code.

This convention is not abstraction for its own sake. It is a guardrail: public names say "project runtime model," private names say "platform substrate," and OS-specific names appear only inside backend documentation and backend code.

## DESIGN PRINCIPLES

- Tools call a model, not a thread. The public surface is the task pool and the I/O loop. Raw thread creation, mutexes, and wait/wake are a private substrate, not the tool-facing contract.
- Zero cost when unused. A tool that does not call a concurrency model links none of it. Single-threaded binaries are byte-for-byte unaffected.
- Serial is a first-class backend. Every parallel API has a correct inline implementation that runs on the calling thread. A missing native backend means "slower," never "broken."
- No external dependency, ever. No libc, no pthreads in the normal path, no `libSystem`/dylib imports on macOS. Synchronization uses compiler atomic builtins plus one raw kernel wait/wake syscall per platform.
- Reuse threads, never churn them. Threads are created once into a pool sized to the machine. Per-chunk create/join is treated as a bug, not an API.
- Make memory parallelism explicit and lock-free by default. Workers own per-worker arenas keyed by a worker index, so the hot parallel path needs no allocator lock.
- No hidden thread-local state. Per-worker context is passed explicitly as an index; the design uses no compiler TLS, no `_Thread_local`, and no errno-like per-thread globals.
- Prefer boring, benchmarked mechanics. Acquire/release atomics plus a single OS wait/wake primitive. No clever lock-free data structures unless a benchmark on a real workload justifies the complexity.
- Keep unsupported platforms honest. A backend that cannot yet start native threads runs the serial backend and says so under introspection; it never silently falls back to a foreign runtime.

## LAYERING

The system is two layers. Tools live entirely in Layer 1.

Layer 1 is the model layer in `src/shared`. It contains the task pool (`rt_parallel_for`, task submit/wait) and the I/O loop. It is platform-independent C built only on Layer 0 and the runtime allocator. This is the only layer tool code includes.

Layer 0 is the substrate, split between `src/platform/*` and a small shared atomics header. It provides exactly three capabilities: start and join a raw worker thread, wait/wake on a 32-bit word, and atomic load/store/CAS/fetch with explicit memory order. Everything in Layer 1 — pools, queues, barriers, the serial fallback — is expressed in terms of those three capabilities, so a new platform is brought up by implementing the substrate and nothing else.

```
        tool code
            |
   +--------+--------+
   |  Layer 1 models |   src/shared/concurrency, src/shared/io_loop
   |  pool, parallel |
   |  _for, io loop  |
   +--------+--------+
            |
   +--------+--------+
   |  Layer 0 substrate
   |  thread create/join
   |  wait/wake on word
   |  atomics
   +--------+--------+
      |        |
   Linux    macOS              (+ hosted POSIX for verification)
  clone/   bsdthread
  futex    /__ulock
```

Mutexes, semaphores, and condition variables still exist, but as private Layer 1 building blocks used to implement the pool and queue. They are not part of the tool-facing API, because making them public invites exactly the hand-rolled, error-prone, per-tool concurrency this design exists to prevent.

## LAYER 1: THE TASK POOL

The task pool is the primary concurrency interface and the answer to CPU parallelism. A pool owns a fixed set of worker threads, created once, sized by default to the machine's usable core count. Work is pushed to the pool and the caller waits for completion; threads are never created or destroyed per unit of work.

The public API is small and intentionally fork/join shaped. Objects are project-owned runtime types rather than external ABI objects; their definitions live in `src/shared/concurrency.h`, and their layout is not a stable ABI.

```
typedef struct RtTaskPool RtTaskPool;
typedef struct RtTaskGroup RtTaskGroup;

/* Lifecycle. worker_count == 0 selects the detected core count.
   A pool with effective width 1 is the serial backend and allocates no threads. */
int      rt_task_pool_init(RtTaskPool *pool, unsigned worker_count);
void     rt_task_pool_destroy(RtTaskPool *pool);
unsigned rt_task_pool_width(const RtTaskPool *pool);
RtArena *rt_task_pool_worker_arena(RtTaskPool *pool, unsigned worker_index);
void     rt_task_pool_reset_stats(RtTaskPool *pool);
void     rt_task_pool_get_stats(const RtTaskPool *pool, RtTaskPoolStats *stats_out);

/* Data parallelism: run body(begin, end, worker_index, arg) over [0, count)
   split into chunks, and return only when every chunk has completed.
   A nonzero body result is recorded and returned after in-flight work joins. */
typedef int (*RtParallelBody)(size_t begin, size_t end, unsigned worker_index, void *arg);
int rt_parallel_for(RtTaskPool *pool, size_t count, size_t min_chunk,
                    RtParallelBody body, void *arg);

/* Irregular fan-out: submit independent tasks, then wait for the batch. */
typedef int (*RtTaskFn)(unsigned worker_index, void *arg);
int rt_task_group_begin(RtTaskPool *pool, RtTaskGroup *group);
int rt_task_group_reserve(RtTaskGroup *group, size_t capacity);
int rt_task_group_submit(RtTaskGroup *group, RtTaskFn fn, void *arg);
int rt_task_group_wait(RtTaskGroup *group);
```

Two properties make this the right shape for this repository. First, every body and task receives an explicit `worker_index` in `[0, width)`. That index is how a worker reaches its own arena, scratch buffer, or output slot without any thread-local storage — the per-thread state problem is solved by passing the one integer that matters. Second, `rt_parallel_for` and `rt_task_group_wait` are structured: they return only when the work is done, so there is no detached lifetime, no cancellation unwinding, and no orphaned thread to leak. A tool's parallel region begins and ends on one stack frame.

The `min_chunk` argument is a lower bound, not an exact claim size. The pool may raise very small requested chunks to keep the total number of atomic chunk claims bounded relative to the worker count. That keeps accidental `min_chunk=1` calls from turning the shared queue index into the workload, while still leaving enough chunks per worker for ordinary load balance.

The return values deliberately stop short of cancellation semantics. If a worker reports an error, the pool records the first nonzero result, lets already-started work reach the structured join boundary, and returns that result to the caller. That is enough for malformed input, allocation failure, and tool diagnostics without making every parallel region a cancellation-unwind problem.

A typical use merges without any shared-state locking at all:

```
/* Each worker accumulates into its own slot; the caller reduces after the join. */
rt_parallel_for(pool, block_count, 1, hash_block_range, slots);
for (unsigned w = 0; w < rt_task_pool_width(pool); w++) combine(total, slots[w]);
```

## THE SERIAL BACKEND

A pool of effective width 1 runs every task inline on the calling thread, in order, allocating no worker threads and touching no wait/wake primitive. This is not a degraded mode; it is a supported, tested backend with first-class status.

The serial backend does three important jobs. It is the universal fallback: a platform whose native thread substrate is not yet implemented still runs every threaded tool correctly, just on one core, with no source changes and no per-tool process-pool workaround. It is the determinism mode for testing: forcing width 1 makes any parallel tool reproducible, so correctness tests do not depend on scheduling. And it is the honest small-N path: when `count` is below the parallel threshold, `rt_parallel_for` runs inline rather than paying pool wakeup latency, so the model never makes small inputs slower.

Because serial execution is real, tools are written once against the pool API and are correct everywhere on day one. Native backends become pure performance upgrades that change timing, never behavior.

## MEMORY: PER-WORKER ARENAS

The allocator coupling is the part most threading designs get wrong, so this design makes it structural rather than advisory. The default runtime heap stays exactly as described in [memory](memory.md): single-threaded and unlocked, so non-parallel tools pay nothing.

A pool optionally owns one arena per worker, indexed by `worker_index`. Inside a parallel region, a worker allocates from its own arena, which no other worker can touch, so the common parallel allocation path takes no lock and suffers no contention. Results are merged after the join by the caller, on the main thread, using the normal heap. This makes the fast path lock-free by construction instead of by discipline.

For the rarer case where workers genuinely share one allocation domain, the runtime still offers the opt-in global allocator lock mode. The ordering of preference is deliberate: pass preallocated per-worker state first, use per-worker arenas second, and only enable the shared global lock when a benchmark shows the merge-after-join structure cannot express the workload. A design that forces global allocator locking to get any parallelism is considered a regression even if it is faster on one tool.

The current macOS project-linked threading bring-up uses the platform-mutex allocator lock for the build path that exercises native workers. That is a safety bridge discovered from `zip` and `threadread`, not the final preferred shape for migrated tools. New conversions should still try to keep worker bodies allocation-light or per-worker allocated so the global lock is not the scalability limit.

## LAYER 1: THE I/O LOOP

The I/O loop is the answer to I/O concurrency and the reason most "make this server threaded" requests should not create threads at all. It is a single-threaded readiness loop that multiplexes many sockets or files and runs registered callbacks as they become ready.

```
typedef struct RtIoLoop RtIoLoop;
typedef void (*RtIoReady)(int fd, unsigned events, void *arg);
typedef void (*RtIoTimer)(void *arg);
typedef void (*RtIoDeferred)(void *arg);

int  rt_io_loop_init(RtIoLoop *loop);
void rt_io_loop_destroy(RtIoLoop *loop);
int  rt_io_loop_add(RtIoLoop *loop, int fd, unsigned events, RtIoReady cb, void *arg);
int  rt_io_loop_modify(RtIoLoop *loop, int fd, unsigned events);
int  rt_io_loop_remove(RtIoLoop *loop, int fd);
int  rt_io_loop_timer(RtIoLoop *loop, unsigned long long delay_ms, RtIoTimer cb, void *arg);
int  rt_io_loop_defer(RtIoLoop *loop, RtIoDeferred cb, void *arg);
int  rt_io_loop_run(RtIoLoop *loop);     /* until no registrations remain or stop is set */
void rt_io_loop_stop(RtIoLoop *loop);
```

Because everything runs on one thread, there is no shared mutable state across handlers, no mutex, no allocator locking, and no per-connection stack. A downloader fetching several files, or an `httpd` serving many clients, becomes a set of small state machines driven by readiness — typically smaller in both code and memory than the worker-per-client equivalent, and with none of the synchronization surface. Timers and deferred callbacks belong to the model because retry backoff, connection timeouts, and "finish this after the current callback" behavior otherwise get reinvented inside each tool. When such a server also has a genuinely CPU-bound step, it hands that one step to the task pool and continues driving I/O.

The current loop is the portable backend: it keeps registrations, timers, and deferred callbacks in runtime-owned arrays and waits through `platform_poll_fds`. That is enough to validate the model without creating threads for I/O. Native `epoll` on Linux and `kqueue` on macOS remain planned backend swaps behind the same `rt_io_loop_*` interface, with `io_uring` available later only if a real workload justifies the extra backend.

## LAYER 0: THE SUBSTRATE

Layer 0 is the only code that touches the kernel's threading and synchronization syscalls. It exposes three capabilities and no more.

Raw worker threads start and join a single worker on a page-backed stack the platform owns and releases on join. These calls live in a private substrate header, not in the tool-facing platform API. Pools are the only expected caller; tool code is not meant to use them directly.

```
typedef int (*PlatformWorkerMain)(void *arg);
int platform_worker_thread_start(PlatformWorkerThread *t, PlatformWorkerMain entry, void *arg, size_t stack_size);
int platform_worker_thread_join(PlatformWorkerThread *t, int *result_out);
```

Wait/wake on a word is the one synchronization primitive every backend must provide, and it is the unifying insight that keeps the design portable and libc-free. Linux `futex` and macOS `__ulock_wait`/`__ulock_wake` are the same primitive with different names: block a thread while a 32-bit word still equals an expected value, and wake one or all waiters after changing that word. Mutexes, semaphores, the pool's idle/wake protocol, and join itself are all expressed on top of this single operation.

```
void platform_wait_word(volatile unsigned int *word, unsigned int expected);
void platform_wake_word_one(volatile unsigned int *word);
void platform_wake_word_all(volatile unsigned int *word);
```

Atomics are compiler builtins, not a library. The current implementation uses `__atomic_*` directly with explicit acquire/release/acq-rel ordering in the pool and platform synchronization code. A tiny shared wrapper may still be useful if more code starts spelling these operations by hand, but no `<stdatomic.h>` and no runtime support code are required.

## LINUX BACKEND

Linux is the reference substrate for the original implementation and has native worker support on x86-64 in `src/platform/linux/thread.c`.

Thread creation uses `clone` with `CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID`. Stacks come from `platform_allocate_pages`; join waits on the kernel-cleared TID via the wait/wake primitive and releases the stack. The existing x86-64 assembly trampoline stands; the remaining Linux substrate work is the aarch64 clone trampoline so both project architectures share one pool.

Synchronization uses `futex`. Operations should use the process-private futex flag once the constants and tests are in place. The I/O loop currently uses the portable poll backend; `epoll` and, later, `io_uring` can replace that behind `rt_io_loop_*` if benchmarks justify them. Core count for default pool sizing comes from `sched_getaffinity` so the pool respects cgroup and affinity limits rather than raw CPU count.

## MACOS BACKEND

The project-linked macOS AArch64 build now has a native worker substrate without adding a libc, pthread, or `libSystem` dependency to the final tool binaries. The code lives in `src/platform/macos/freestanding.c`; hosted POSIX builds remain verification paths and are not the design center.

Synchronization on Darwin uses `__ulock_wait`/`__ulock_wake`, the kernel wait/wake syscalls that sit underneath `os_unfair_lock`. They are the direct analog of `futex`, so the mutex, semaphore, and pool logic written for Linux is reused unchanged with only the wait/wake backend swapped. This keeps macOS synchronization independent of Mach ports, pthreads, and any foreign runtime.

Thread creation on Darwin uses `bsdthread_create`, the raw syscall that backs `pthread_create`. The backend first attempts `bsdthread_register` with project-owned start trampolines. If that returns `EINVAL`, the process is already registered by dyld's pthread startup code; this is treated as a usable pre-existing registration rather than a fatal error. In that path the backend initializes the small subset of the pthread-shaped block that dyld's `__pthread_start` reads: the signed pthread signature, `fun`, `arg`, stack metadata, errno storage, and the TSD self/errno slots. The `fun` pointer is a project-owned no-return worker wrapper, so the new thread runs the task-pool entry point, publishes `clear_tid`, wakes joiners, and exits with raw `bsdthread_terminate` instead of returning into libpthread cleanup.

Join waits on the worker's `clear_tid` word with `__ulock_wait` and releases the page-backed stack after the worker publishes exit. Default pool sizing uses `sysctlbyname("hw.logicalcpu")` through the project-linked raw syscall shim, and benchmark timing uses the AArch64 virtual counter registers rather than wall-clock `gettimeofday`.

The I/O loop on macOS currently uses the portable poll backend. A `kqueue` backend still makes sense as a later targeted replacement, and it can land independently of worker thread creation.

## HOSTED BACKENDS

Hosted POSIX builds may implement the substrate with pthreads as a bring-up and verification convenience. This is never the contract: hosted code obeys the same Layer 1 semantics so tools cannot accidentally depend on pthread behavior that the freestanding substrate does not provide. The hosted path exists to cross-check the project substrate, not to define it.

## PLATFORM EXPECTATIONS FOR TOOL CODE

Tool code should not special-case Linux and macOS threading behavior. A tool initializes `RtTaskPool`, receives either a native width or the serial backend, and runs the same worker body in both cases. If one platform does not yet have native workers for an architecture, the serial backend is the compatibility layer.

Do not include OS thread headers, call pthreads, call C11 thread APIs, or reach into `src/platform/*` from a tool. Linux `clone`/`futex`, macOS `bsdthread`/`__ulock`, and hosted pthreads are backend details. The tool-facing contract is `src/shared/concurrency.h`.

Do not use platform conditionals to change output order, error handling, chunk boundaries, or file-format behavior. A Linux native-width run, a macOS native-width run, and a width-1 serial run should differ only in timing and resource use. If platform-specific behavior is needed to access files, sockets, clocks, or memory, route it through the existing `platform.h` layer outside the parallel model.

When validating a tool, prefer the freestanding/project-linked build that matches the platform substrate: `make freestanding` for the normal project path, focused `make build/macos-aarch64/<tool>` checks on local macOS AArch64, and the relevant freestanding Linux build on Linux. Hosted POSIX can catch ordinary C mistakes quickly, but it cannot prove that no-libc thread startup, raw wait/wake, compact executable layout, or project allocator policy is healthy.

## TLS AND THREAD-LOCAL STATE

The design uses no thread-local storage. The pool gives each worker an explicit `worker_index`, and every per-worker resource — arena, scratch buffer, output slot, RNG state — is reached through that index. This deliberately avoids compiler-emitted TLS, `_Thread_local`, pthread keys, and any errno-like hidden per-thread global, all of which would complicate the freestanding startup and the no-`libSystem` macOS path. If a future subsystem ever truly needs implicit thread-local state, it gets its own explicit project design; it is never acquired as a silent side effect of starting a thread.

## SIGNALS AND PROCESS EXIT

Concurrency does not introduce a broad signal model. Signal handling stays narrow and platform-owned. Pool workers always return through their task function and are joined by the structured `rt_parallel_for`/`rt_task_group_wait` boundary before the region ends; a worker never calls `platform_exit`, which is reserved for whole-process termination. The I/O loop stops through `rt_io_loop_stop`, and long-running services define their own shutdown flag or stop event rather than relying on signals to unwind worker state.

## ERROR HANDLING

Pool initialization can fail only at the substrate boundary (thread or page allocation). `rt_task_pool_init` returns nonzero on failure, and a tool may respond by retrying with width 1, which is the serial backend and cannot fail. Because `rt_parallel_for` and `rt_task_group_wait` are structured and join everything they start, there is no partial-failure cleanup for tool code to get wrong: either the region completed or the pool was never built.

Substrate wait/wake operations are fatal-or-impossible in normal use and stay `void` to keep the hot path compact. If a real backend surfaces a recoverable synchronization failure, it gets a narrow diagnostic hook rather than a return code threaded through every lock and unlock.

## SECURITY CONSIDERATIONS

The task pool is a throughput mechanism, not a security boundary. Workers share the process address space, file descriptors, heap, runtime state, and privileges of the main thread. Running a parser, decompressor, crypto operation, archive writer, or executable handler inside a worker does not isolate malformed input or make the operation safe for production use.

Threading can make ordinary C bugs harder to reproduce and easier to trigger under load. Data races, stale pointers, double frees, lifetime mistakes, unsynchronized diagnostics, shared-output corruption, allocator bugs, wait/wake bugs, and worker-stack exhaustion can become memory-corruption, hang, or denial-of-service issues. Treat every worker body that touches untrusted input as security-sensitive even if the serial version already passed its tests.

Resource amplification is the most likely user-visible security consequence. Each native worker has a stack, task groups allocate task arrays, and many practical migrations use per-index output buffers so the main thread can write in deterministic order. A malicious or simply large input can therefore consume more CPU and memory at native width than at width 1. Tools that buffer full compressed input, decoded blocks, encrypted chunks, archive payloads, or parser results should define size thresholds and serial fallback behavior before being considered robust.

Environment worker knobs such as `NEWOS_ZIP_WORKERS`, `NEWOS_HASH_WORKERS`, `NEWOS_SORT_WORKERS`, `NEWOS_EXPACK_WORKERS`, `NEWOS_PGPMSG_WORKERS`, and `NEWOS_BUNZIP2_WORKERS` are debugging and tuning controls, not trust controls. They are capped by `RT_TASK_POOL_MAX_WORKERS`, but they should not be exposed as a way for untrusted users to force resource consumption in privileged or service-style contexts.

Developer rule: a threaded migration is not complete until malformed input, allocation failure, width 1, and at least one native worker width have all been exercised. Security-sensitive tools should prefer fail-closed behavior after the join boundary: no partial final output, no unchecked worker result slots, no worker-owned user-facing writes unless the output is explicitly diagnostic, and no assumption that an error in one task cancelled the rest.

## TESTING PLAN

Concurrency is not ready for general tool use until it is tested in the primary freestanding target, and the serial backend makes that testing tractable.

Required pool tests include running `rt_parallel_for` over a large count and verifying every element was visited exactly once; reducing per-worker arena results into a known total; an irregular `rt_task_group` fan-out that completes fully; confirming width-1 (serial) and width-N produce identical results for the same input; verifying small-N inputs run inline without pool wakeup; propagating the first nonzero worker result after the join; and confirming a destroyed pool leaks no stack pages. Required I/O-loop tests include many concurrent loopback connections driven to completion on one thread, correct add/modify/remove behavior, timer and deferred-callback ordering, and clean `rt_io_loop_stop`. Substrate tests cover wait/wake correctness and the mutex/semaphore building blocks directly.

Every test runs under the serial backend first (deterministic, all platforms), then under the native backend on Linux x86-64 and project-linked macOS AArch64, then hosted POSIX. Linux AArch64 joins that native matrix once its clone trampoline lands. Stress tests cover high-contention wait/wake churn, repeated pool create/destroy cycles, and arena-heavy parallel workloads.

## TOOL MIGRATION PRACTICES

Migrating a tool to concurrency starts by choosing the right model. CPU-bound, splittable work belongs in `rt_parallel_for` or `RtTaskGroup`; I/O-bound waiting belongs in `RtIoLoop`; raw platform threads are not a migration target. If the tool needs both, keep the I/O state machine single-threaded and hand only the expensive CPU phase to a task pool.

Keep the existing serial behavior as the correctness reference. A migrated tool should be able to force width 1, either by passing worker count 1 to the pool or by a tool-specific environment knob during bring-up, and width 1 should produce byte-identical output to the old implementation. Phase tests should exercise width 1 and at least one native width, because width 1 catches behavior regressions while native width catches lifetime, ordering, and allocator mistakes.

Build the parallel region around immutable inputs and per-worker outputs. Collect file lists, metadata, option state, and output ordering on the main thread first. In the worker body, avoid mutating shared structures; write into a slot owned by the input index or into storage addressed by `worker_index`, then merge in a deterministic order after `rt_parallel_for` returns. Archive writers and formatters should prepare payloads in parallel but write headers, payloads, indexes, and diagnostics on the main thread unless the file format explicitly permits unordered output.

Treat `min_chunk` as a cost hint, not a promise. Pick a chunk size large enough that useful work dominates dispatch overhead. The runtime will raise tiny chunks to cap atomic claim traffic, but callers should still avoid exposing millions of trivial chunks when the work can naturally be batched by file, block, row group, section, or task batch.

Prefer preallocation and per-worker storage over global heap traffic. The best parallel body does not allocate at all. The next best one allocates from per-worker arenas or from pre-sized per-index result slots. Shared global allocator locking is a fallback for workloads that are hard to express that way, and it must be benchmarked under contention before becoming the default shape for a tool.

Reserve irregular batches before submitting them. If a task group knows or can cheaply estimate its task count, call `rt_task_group_reserve` once before `rt_task_group_submit` loops. That keeps the submission path from benchmarking reallocations instead of task execution.

Error handling stays structured. A worker can return nonzero, but it cannot assume cancellation of other already-started chunks. Worker bodies must leave their own output slot either complete or clearly marked failed, and the main thread performs cleanup after the join boundary. Do not emit user-facing partial output from workers unless the output is explicitly diagnostic and ordering does not matter.

Measure correctness, speed, and size together. Use `threadbench --stats`, `threadstress`, and workload-specific fixtures to check active workers, effective chunk size, imbalance, and error propagation. Use the project `timeout` tool around new high-width tests so a hang becomes useful evidence rather than a stuck test run. For tools that read or allocate in workers, `experimental/threading/threadread` is the current focused reproducer for macOS-style concurrent read/allocation pressure.

Implementation checklist for a new threaded tool:

1. Save a baseline benchmark before editing the tool. Put ad-hoc measurement output under `tests/tmp`, include the build directory, fixture description, worker widths, and enough command lines that the result can be repeated later.
2. Choose the split unit before choosing the API. Use `rt_parallel_for` for dense, uniform ranges such as files, rows, blocks, archive members, or sections. Use `RtTaskGroup` for irregular jobs such as candidate encoders, recipient packets, format blocks discovered by a scanner, or other work whose count is known only after setup.
3. Add a bring-up worker knob named `NEWOS_<TOOL>_WORKERS` unless the tool already has a broader project convention. Width 1 must force the serial backend. Widths greater than 1 should be capped to a conservative default until benchmarks show a better cap.
4. Build an immutable job array on the main thread. Normalize options, collect input paths or block metadata, reserve output slots, and compute deterministic output order before starting the pool.
5. Keep worker functions small and boring. A worker should read its job, write only its assigned output slot or per-worker scratch, mark success or failure, and return. It should not print final user output, mutate global format state, create threads, depend on TLS, or assume another task will be cancelled after its own error.
6. Treat serial discovery and serial merge as part of the workload. A naive block scanner, expensive pre-pass, or single-threaded final merge can erase a good parallel body. Measure setup, worker time, and write/merge time separately when speedups are disappointing.
7. If `rt_task_pool_init` fails at a requested native width, retry or fall back to width 1 rather than failing a tool that can still run correctly on the serial backend.
8. Add focused phase coverage that compares width 1 against at least one native width. The test should check byte-identical output for deterministic tools, correct diagnostics for malformed input, and timeout-bounded completion for high-width bring-up cases.
9. Rebuild and test the project-linked target on macOS and the relevant freestanding Linux target when available. Hosted POSIX is useful for bring-up, but it is not proof that the no-libc substrate and allocator policy are healthy.
10. Re-run the baseline benchmark after the change, then inspect binary size, small-input latency, best width, and user time. A lower wall time with much higher user time is only acceptable when the workload is explicitly throughput-oriented and small inputs still stay on the inline path.

For LLM-assisted migrations, the first search should be for existing examples of the model rather than for raw thread code. Good current examples are checksum multi-file fan-out with `rt_parallel_for`, `zip` payload preparation, `sort` chunk sorting, `expack` candidate groups, `pgpmsg` recipient and AEAD chunk groups, and standard `bunzip2` block groups. New tool code should look like those patterns unless a measured workload proves it needs a different shape.

## CURRENT STATUS AND ROADMAP

The migration plan is now past the first-tool stage. The model exists, native worker backends exist on the main development platforms, and several real tools exercise both `rt_parallel_for` and `RtTaskGroup`. Keeping the plan in this document helps avoid drifting back into per-tool thread designs as more tools migrate.

Current status:

1. The serial backend is implemented and remains the correctness baseline. Width 1 runs inline and is used as the deterministic path for tests and debugging.
2. `RtTaskPool`, `rt_parallel_for`, `RtTaskGroup`, task-group reservation, and pool statistics are implemented in `src/shared/runtime/concurrency.c`.
3. Native worker support exists for freestanding Linux x86-64 and project-linked macOS AArch64. Linux AArch64 still falls back to serial execution until its clone trampoline exists.
4. The portable I/O loop exists in `src/shared/runtime/io_loop.c` and currently runs over `platform_poll_fds`. Native `epoll` and `kqueue` backends have not landed yet.
5. Experimental measurement lives under `experimental/threading`: `threadbench`, `threadstress`, `threadcompress`, `threadread`, `benchcmp.sh`, `benchguide.sh`, and `report.sh`.
6. The macOS project-linked build uses the platform-mutex allocator lock mode for thread-safe global heap access in the current threaded bring-up path. This fixed hangs seen with the atomic spin allocator lock under `zip` and `threadread`-style concurrent read/allocation pressure.

Real tool migrations now cover several workload shapes:

| Workload | Current shape | Worker knob | Main lesson |
| --- | --- | --- | --- |
| `zip` payload preparation | `rt_parallel_for` over input entries, serial archive write | `NEWOS_ZIP_WORKERS` | Prepare payloads in parallel, keep format output ordered on the main thread. |
| `md5sum`, `sha1sum`, `sha256sum`, `sha512sum` | `rt_parallel_for` over input files | `NEWOS_HASH_WORKERS` | Per-file slots give large wins with little shared state. |
| `sort` | `RtTaskGroup` chunk sorts followed by deterministic merge | `NEWOS_SORT_WORKERS` | Parallel sort is useful, but serial merge and comparison cost cap speedup. |
| `expack` | `RtTaskGroup` over codec candidates | `NEWOS_EXPACK_WORKERS` | Irregular candidate work fits task groups; bad candidate portfolios can dominate more than threading. |
| `pgpmsg` | `RtTaskGroup` over recipients and v2 AEAD chunks | `NEWOS_PGPMSG_WORKERS` | Chunk parallelism is strong, and it exposes crypto primitive cost quickly. |
| standard `bunzip2` | `RtTaskGroup` over discovered bzip2 blocks | `NEWOS_BUNZIP2_WORKERS` | Block parallelism works only after serial scanning overhead is under control. |

Remaining roadmap:

1. Keep broadening real-tool coverage into image, compiler, linker, SQL, and archive workloads that can use per-index or per-worker output slots.
2. Finish native Linux AArch64 worker thread support so both Linux project architectures share the same native pool behavior.
3. Add native `epoll` and `kqueue` I/O-loop backends behind `rt_io_loop_*`, then migrate one I/O-bound tool such as a downloader or server loop and compare it against the existing polling shape.
4. Move hot worker allocation paths toward preallocated output arrays and per-worker arenas so the global allocator lock becomes rare rather than structural.
5. Standardize worker-count parsing and benchmark reporting so each tool does not reinvent the same `NEWOS_<TOOL>_WORKERS` plumbing differently.
6. Tighten build granularity so tools that never use concurrency do not inherit thread-safe allocator policy or concurrency object code merely because one threaded tool in the same build tree needs it.
7. Keep tracking binary size, small-input latency, active-worker counts, chunk imbalance, memory footprint, and timeout-bounded stress behavior for every migrated tool.

The long-term rule remains that tools which do not opt into a concurrency model should be untouched and unmeasurably affected. Any temporary build-wide hardening, such as the macOS project-linked allocator lock, should be treated as an implementation gap to narrow once the threaded tool set and allocator usage patterns are better understood.

## DISCOVERIES AND OPEN PROBLEMS

The native macOS worker substrate is real enough for useful work. Recent macOS AArch64 reports show CPU `mix` scaling to roughly 11x at width 16, compression-shaped work around 10x, and `zip -6` around 6x to 7x on a large synthetic input. Memory-shaped work improves but plateaus earlier, with width 8 often near width 16; this is the expected bandwidth ceiling rather than a pool failure.

Automatic batching matters. Treating `min_chunk` as a lower bound fixed the pathological tiny-chunk case: a caller can request `min_chunk=1`, but the pool raises the effective chunk size so dispatch claims stay proportional to worker count. The stats output makes this visible and should remain part of benchmark review.

The original `zip` migration exposed allocator reality. `zip` and the focused `threadread` reproducer showed that a shared global allocator under concurrent read/allocation pressure can hang or crash if protected only by the simple atomic spin lock on macOS. The platform mutex lock, backed by the same wait/wake substrate as the task pool, fixed the reproducer and stabilized high-width `zip` runs. That is a useful fix, but it is also a warning: the preferred design is still to avoid the shared allocator in hot worker bodies.

Hangs are diagnostics, not just failures. New threaded tests should be wrapped with the project `timeout` tool during bring-up. A timed-out run that leaves no archive bytes, no completed chunks, or a partial worker-counter snapshot often points directly at the layer that is blocked.

Real tool migrations confirmed the core pattern. The most reliable structure is immutable input metadata, per-index or per-worker output slots, and a serial ordered merge or write after the join. That pattern has held across file hashing, archive payload preparation, sort chunks, executable-pack candidates, PGP recipient packets, PGP AEAD chunks, and bzip2 blocks. When a proposed migration cannot be expressed this way, treat that as a design smell and look for a higher-level split before introducing shared mutable state.

Serial setup can be the real bottleneck. The standard bzip2 migration first showed almost no wall-time improvement because the block scanner was too expensive; replacing it with a rolling 48-bit scanner unlocked the parallel decode. The general lesson is that a threaded worker body is only one phase of the tool. Discovery, validation, merge, formatting, and final writes need their own measurements.

Threading exposes primitive costs. `pgpmsg` chunk parallelism made AES-GCM the obvious hot path, and the later GHASH table optimization produced a larger CPU reduction than key-schedule reuse alone. When a threaded tool stops scaling, inspect the shared primitive before adding more workers.

Not every CPU-looking workload wants internal threading. Single-frame zstd decompression has output dependencies that make intra-frame splitting unattractive; hot-loop improvements and higher-level parallelism across independent frames or archive members are better targets. Project mini-bzip streams are simple enough that the standard bzip2 block strategy does not apply. A negative threading result is useful evidence, not a failure of the model.

Memory growth is now the main practical concern. Per-block and per-task outputs are deterministic and easy to reason about, but tools such as threaded `bunzip2` may buffer compressed input plus decoded blocks before writing in order. Large-file thresholds, bounded staging, and serial fallback policies need more attention before every block format adopts this shape.

The largest current shortcomings are I/O-loop proof, platform parity, allocator granularity, and build granularity. The I/O-loop model has not yet been proven by a migrated server or downloader, Linux AArch64 native workers are still missing, the macOS allocator policy is coarser than the long-term zero-cost goal, and shared compression objects may need splitting if threaded entry points pull concurrency into otherwise serial tools. These are engineering gaps in the migration, not reasons to abandon the model.

## BENCHMARKING

Concurrency changes are judged against real workloads and binary size together, never speed alone.

Useful measurements include the binary-size delta for a tool using `rt_parallel_for` versus its single-threaded and process-pool variants; `rt_parallel_for` speedup versus core count, including the small-N inline path; per-worker-arena (lock-free) versus shared-global-lock allocation throughput in worker-heavy tools; wait/wake uncontended and contended cost across the Linux and macOS backends; I/O-loop connection throughput and per-connection memory versus a worker-per-client server; and pool memory footprint including stack commitment and release.

For real tool migrations, save a before/after benchmark under `tests/tmp`. The baseline should be captured before code changes, and the post-change run should include width 1 plus the expected native widths. Record real time, user time, system time, input size, output size when relevant, build directory, environment knobs, and the command line. Width 1 after the migration is the new serial reference and should be compared against both the old implementation and the native-width run.

Interpret speedups by phase. A wall-time win with a large user-time increase may be acceptable for large throughput workloads, but it can also reveal wasted work, a serial scanner bottleneck, too many tiny tasks, allocator contention, or poor chunk size. If native width is not faster than width 1, measure discovery, worker body, and merge/write separately before changing the pool.

Recent migrations provide useful benchmark expectations. Multi-file hash sweeps and PGP v2 AEAD chunks can scale strongly because their work units are independent. `sort` improves but is capped by comparison and merge costs. `expack` depends heavily on the candidate portfolio. Standard bzip2 block decode only scaled after scanner overhead was reduced. Single-frame zstd decompression is currently better served by hot-loop optimization or higher-level parallelism than by internal frame threading.

While the runtime is still experimental, measurement lives under `experimental/threading`. `threadbench` is the synthetic benchmark: it reports min/median/p90/max timings, separates pool width from actual active workers, shows requested versus effective chunk sizes, and can print task-pool counters plus per-worker imbalance summaries with `--stats`. `threadstress` repeatedly creates pools, runs randomized parallel ranges and batched task groups, checks forced-error propagation, and reports aggregate counters. `benchcmp.sh` compares two saved report files by row key and prints median-time ratios and speedup deltas. `benchguide.sh` parses width-sweep sections and reports the fastest width plus the smallest width within 5 percent of best median time, which is the current experimental form of adaptive width guidance. These tools are intentionally outside `src/tools` until the measurement surface settles.

A design that speeds up one large workload but grows tiny tools, forces global allocator locking, or makes the single-threaded common case pay anything is not a win.

## NON-GOALS

- Do not expose raw threads, mutexes, or semaphores as the tool-facing API; tools call the pool and the I/O loop.
- Do not create or destroy threads per unit of work; the pool is created once and reused.
- Do not implement a pthread clone or add C11 threads compatibility unless an in-tree caller truly needs it.
- Do not make the source-level default runtime allocator thread-safe for every target; parallelism should use per-worker arenas, and any global lock mode stays an explicit build or tool choice.
- Do not use thread-local storage; per-worker state is reached through an explicit `worker_index`.
- Do not add detach, cancellation, priorities, scheduling policies, or broad signal routing as baseline features; structured fork/join makes them unnecessary.
- Do not introduce any external dependency, standard-library call, or — on macOS — any `libSystem`/dylib import to obtain threads or synchronization.
- Do not let a single-threaded tool pay any code, allocator, TLS, or startup cost for concurrency it does not use.

## EXPECTED OUTCOME

The intended result is a concurrency layer where:

- single-threaded tools are byte-for-byte unaffected and pay nothing
- tools express CPU parallelism through a reusable task pool and I/O concurrency through an I/O loop, never through hand-rolled threads
- the hot parallel path is lock-free by construction via per-worker arenas, with no thread-local storage
- one wait/wake substrate (`platform_wait_word` over Linux `futex` or Darwin `__ulock`) and compiler atomics carry all synchronization, with no libc and no `libSystem`
- the serial backend makes every threaded tool correct on every platform immediately, so native backends are pure performance upgrades
- macOS and hosted backends implement the same two models without any change to tool code

## SEE ALSO

runtime, platform, memory, build, testing, userland