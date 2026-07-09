# MEMORY

## NAME

memory - project-owned memory primitives, allocation policy, and benchmark plan

## DESCRIPTION

The memory runtime is not a libc compatibility layer. It is the project-owned
memory substrate for the freestanding userland. All current callers live in this
repository, and they can be changed together when a better memory model becomes
available.

The design goal is therefore not to preserve accidental `malloc(3)` behavior.
The design goal is to make the resulting binaries small, fast, predictable, and
well matched to the tools this repository actually builds.

This page describes the design now used by `src/shared/runtime/memory.c` and the
nearby platform page primitives. Some migration items, such as broad caller
adoption of array helpers and arenas, remain ongoing work.

## WORKLOAD MODEL

The userland has several distinct allocation patterns:

- tiny tools that allocate rarely or not at all, such as `true`, `false`,
  `echo`, `basename`, and similar argument-only commands
- streaming filters that mostly use fixed buffers, such as text, checksum, and
  compression tools
- parser-heavy tools that build temporary state while processing one input,
  such as XML, image metadata, archive readers, regex users, and command parsers
- compiler, linker, SQL, shell, and build tools that have clear phase lifetimes
  and many vectors, tables, strings, and intermediate records
- long-running or interactive tools such as `sh`, `editor`, `mail`, `httpd`,
  `ssh`, `sshd`, and `service`, where temporary memory spikes should not become
  permanent process growth
- optional threaded tools, currently a minority, where allocator locking is
  needed only when the tool actually allocates across threads

No single libc-style heap is optimal for all of these. The runtime exposes the
small set of allocation patterns the project wants, then callers can move toward
those patterns.

## DESIGN PRINCIPLES

- Keep binary size visible. Tiny tools should not pull in arena, heap, or
  locking machinery unless they use it.
- Prefer project semantics over libc compatibility. The repository owns every
  caller, so unclear or expensive legacy behavior should not be preserved.
- Make the fast path simple. Small allocations, vector growth, and phase
  allocation should be cheap in code size and CPU time.
- Make overflow handling central. Callers should not repeatedly open-code
  multiplication checks for arrays and vectors.
- Keep OS interaction behind `platform.h`. Page allocation and page release
  belong in the platform layer, not in generic runtime code.
- Make thread safety opt-in. Single-threaded tools should not pay for atomics or
  mutexes in normal allocation paths.
- Benchmark every design change against real tool families, not only synthetic
  allocator loops.

## MODULE SHAPE

The current implementation lives in `memory.c` so the build can adopt the new
allocator without Makefile churn. A future split can move the pieces into:

- `memory.c` - mandatory freestanding memory primitives: `memcpy`, `memmove`,
  `memset`, `memcmp`, plus small helpers that do not require heap state
- `alloc.c` - general heap allocation: `rt_malloc`, `rt_realloc`, `rt_free`,
  and array-aware allocation helpers
- `arena.c` - optional phase allocator for parser/compiler/linker-style
  workloads

That split would let section garbage collection and source-group wiring keep
small binaries from paying for allocation modes they do not use. The exact file
names may change, but the dependency direction should not: primitives are
lowest, general heap is above primitives, and arenas sit beside the heap as a
separate lifetime model.

## MEMORY PRIMITIVES

The project must provide its own `memcpy`, `memmove`, `memset`, and `memcmp` for
freestanding builds and for hosted builds compiled with `-fno-builtin`.

These primitives should be:

- safe against compiler recursion, where the compiler lowers a loop back into a
  call to the same primitive
- fast enough for file, XML, image, compression, compiler, linker, and terminal
  workloads
- small enough to remain acceptable in tiny binaries
- deterministic and independent of libc

The current implementations copy and compare machine words where alignment and
remaining length permit, then finish with byte tails. They retain conservative
byte paths and are compiled with the freestanding anti-recursion policy. Changes
must still be checked on hosted, Linux freestanding, and macOS project-linked
builds because compiler loop recognition can otherwise turn a primitive back
into a call to itself.

## GENERAL HEAP

The general heap is a compact size-class allocator, not a first-fit coalescing
malloc clone.

Small allocations use fixed size classes. The current class set is:

    16, 32, 64, 128, 256, 512, 1024, 2048

The exact classes should continue to be benchmarked. The current behavior is:

- `rt_malloc` rounds small requests to a size class and pops a slot from that
  class free list
- when a class is empty, the allocator obtains a page run from the platform and
  carves it into slots
- `rt_free` of a small allocation pushes the slot back onto the class free list
- large allocations are page-backed directly rather than stored in small bins
- large freed allocations enter a bounded cache or return pages to the platform

The release defaults bound the large-allocation cache to 8 MiB and 32 mappings.
These are implementation defaults, not API guarantees, and can be overridden at
compile time with `RT_LARGE_CACHE_MAX_BYTES` and
`RT_LARGE_CACHE_MAX_COUNT`. A public cache-trim operation has not been added;
long-running-service measurements should decide whether one is necessary.

This model favors speed, small code, and predictable behavior over perfect heap
packing. That trade is appropriate for this userland: many tools are short-lived,
and longer-lived tools benefit more from direct page release for large temporary
buffers than from a complex coalescing general heap.

## PAGE ALLOCATION

The platform contract includes allocation plus release:

    void *platform_allocate_pages(size_t size);
    int platform_free_pages(void *ptr, size_t size);

`platform_free_pages` uses `munmap` on Linux and POSIX/macOS backends, and
`VirtualFree` on Windows. Runtime code keeps sizes page-aligned before calling
either function.

Large heap allocations and arena backing blocks use this interface. This is
important for long-running tools and for high-watermark workloads such as
linking, compiling, parsing large XML, reading image metadata, or handling large
network responses.

## ALIGNMENT

The allocator should guarantee at least 16-byte alignment for normal heap and
arena allocations on 64-bit targets. The block header size must itself be
rounded so that the returned user pointer is aligned; it is not enough to align
only the requested payload size.

If a future caller needs a larger alignment, add a narrow explicit API rather
than quietly increasing the cost of every allocation.

## PROJECT SEMANTICS

The runtime should define project-specific semantics:

- `rt_malloc(0)` returns `0`
- `rt_realloc(0, size)` behaves like `rt_malloc(size)`
- `rt_realloc(ptr, 0)` frees `ptr` and returns `0`
- `rt_free(0)` is allowed
- returned memory is uninitialized unless the API name explicitly says otherwise
- allocation failure returns `0`; callers that cannot recover should report a
  tool-specific diagnostic and fail cleanly

These rules are intentionally smaller than libc's historical edge cases. Current
callers that require a non-null zero-length buffer should be adjusted to request
at least one byte explicitly or to handle `0` as the empty buffer.

## ARRAY ALLOCATION

Array and vector allocation should use checked helpers:

    void *rt_malloc_array(size_t count, size_t item_size);
    void *rt_realloc_array(void *ptr, size_t count, size_t item_size);

These helpers return `0` if `count * item_size` overflows or if allocation
fails. They should become the normal way to allocate vectors, token arrays,
section tables, parsed XML stacks, image metadata records, compiler IR arrays,
linker object records, and similar structures.

Callers should stop open-coding unchecked multiplication before `rt_malloc` or
`rt_realloc`. This is both safer and more compact once repeated patterns are
migrated.

Current ordinary tool code has mostly moved in this direction. Shell parser and
job vectors, editor line arrays, `portscan` positional arguments, `expack`
codec tables, PDF/XML/Git vectors, SQL aggregate state, zip entry lists, sort
line storage, and checksum result arrays all use the checked helpers for
element-count allocation. Byte buffers that grow by byte capacity, such as
streaming input buffers, may still use `rt_malloc` or `rt_realloc` directly when
there is no element-size multiplication to centralize.

The expected effect of this migration is correctness and consistency rather
than a user-visible speedup. Successful allocation sizes are the same as before,
so steady-state runtime and memory use should be unchanged. Binary-size deltas
should be neutral or small: repeated open-coded overflow checks can shrink when
they become helper calls, while simple call sites may grow by a few bytes if the
helper was not otherwise referenced. The behavioral improvement is that
oversized vectors fail cleanly through the same overflow path instead of relying
on each caller to get multiplication and capacity growth checks right.

## ARENAS

Arenas are a first-class runtime facility for phase-lifetime allocation. The API
is:

    typedef struct RtArenaBlock RtArenaBlock;
    typedef struct {
        RtArenaBlock *blocks;
        size_t default_block_size;
    } RtArena;

    void rt_arena_init(RtArena *arena, size_t default_block_size);
    void *rt_arena_alloc(RtArena *arena, size_t size);
    void *rt_arena_alloc_array(RtArena *arena, size_t count, size_t item_size);
    void rt_arena_reset(RtArena *arena);
    void rt_arena_destroy(RtArena *arena);

Arena allocation should be a bump-pointer fast path inside page-backed blocks.
Passing `0` as `default_block_size` selects the runtime default. `rt_arena_reset`
rewinds all allocations in existing backing blocks as a group. Subsequent
allocations scan those reset blocks before obtaining more pages, so a repeated
phase with the same high-water mark reuses its mappings. Blocks remain attached
until `rt_arena_destroy` returns their backing pages to the platform.

Good arena candidates include:

- compiler parse and semantic phases
- linker object, symbol, relocation, and layout phases
- XML document or DTD validation state
- regex compilation state
- image/C2PA metadata scans
- shell parse trees and temporary expansion state
- SQL query execution temporaries

Arenas should not replace every heap use. They fit clear group lifetimes. General
heap allocation remains useful for objects with independent lifetimes, long-lived
state, and APIs that transfer ownership.

## LOCKING

Allocator locking should be selected by build policy, not enabled by default.

The intended modes are:

    NEWOS_RUNTIME_ALLOC_LOCK_NONE
    NEWOS_RUNTIME_ALLOC_LOCK_ATOMIC
    NEWOS_RUNTIME_ALLOC_LOCK_PTHREAD
    NEWOS_RUNTIME_ALLOC_LOCK_PLATFORM

Single-threaded tools should use no allocator lock. Tools that allocate across
project threads should opt into the smallest correct lock mode for the selected
platform. Hosted pthread locking is acceptable for hosted verification builds,
but freestanding targets should avoid pthread assumptions.

This is a deliberate performance and size choice. The common case is a
single-threaded freestanding tool; it should not pay for atomics or mutexes.

The normal Linux x86-64 project-linker build compiles separate no-lock and
atomic-lock `memory.c` objects. It selects the locked object only for current
tools whose worker bodies can allocate: `zip`, `bzip2`, `bunzip2`, `expack`, and
`pgpmsg`. `sort` and the checksum tools use preallocated or fixed worker state
and retain the no-lock allocator. The macOS project-linked build still selects
the platform-mutex allocator globally as a bring-up safety policy; narrowing its
LTO object grouping remains an implementation task rather than an achieved
zero-cost property.

## DEBUGGING MODES

Debug allocation features should be compile-time options, not release defaults.
Useful debug-only features include:

- allocation counters and high-water marks
- red-zone or magic-value checks around heap blocks
- double-free detection where practical
- poison-on-free or poison-on-arena-reset
- optional allocation tracing for a selected tool

These modes are for diagnosing allocator and caller bugs. They should not affect
normal binary size, normal fast paths, or the release ABI.

## MIGRATION PLAN

The core runtime pieces are in place:

- `platform_free_pages` exists in the platform backends
- checked array allocation helpers are available
- the first-fit heap has been replaced by the size-class plus large-page design
- arena support is available for phase-lifetime subsystems
- allocator locking is selected by build policy

The remaining migration should happen in stages:

1. Split memory primitives from heap policy if that improves link behavior.
2. Migrate vector-like call sites to checked array helpers.
3. Migrate phase-lifetime subsystems to arenas one at a time.
4. Wire threaded tools explicitly to the smallest correct allocator lock mode.
5. Tune size classes, arena block sizes, large-allocation cache limits, and
  large-allocation thresholds through benchmark data.

Each stage should keep the full project buildable. Since all callers are in-tree,
the preferred approach is to update callers to the better API rather than keep
compatibility shims forever.

## BENCHMARKING

Allocator changes must be benchmarked against both synthetic and real workloads.
Synthetic tests are useful for isolating allocation cost, but they are not enough.

Required benchmark dimensions:

- final binary size for tiny non-allocating tools
- final binary size for small allocating tools
- full `make freestanding` build time
- startup and steady-state runtime for representative tiny tools
- XML parse/format/validate workloads
- image/C2PA metadata workloads
- compiler and linker workloads, including LTO where practical
- archive/compression workloads that stress memory copying
- long-running process behavior for `sh`, `httpd`, `mail`, or `editor`
- peak resident memory and whether large temporary allocations are returned

Useful comparison builds:

- current first-fit allocator baseline
- size-class heap without arenas
- size-class heap with migrated array helpers
- size-class heap plus arenas for selected parser/compiler/linker workloads
- no-lock versus atomic-lock versus pthread-lock allocator builds
- several class layouts and large-allocation thresholds

Benchmark output should include both speed and size. A faster allocator that
makes tiny binaries noticeably larger is not automatically a win; a smaller
allocator that slows parser-heavy tools substantially is not automatically a win.
The right answer is workload-dependent and should be chosen with project data.

## NON-GOALS

- Do not implement a full libc allocator clone.
- Do not preserve obscure `malloc(3)` edge cases unless an in-tree caller has a
  real need.
- Do not make every allocation thread-safe by default.
- Do not add heavy metadata to release builds for debugging convenience.
- Do not make arenas the only allocation model.
- Do not introduce external allocator code or third-party dependencies.

## EXPECTED OUTCOME

The intended result is a memory layer where:

- tiny tools stay tiny
- common heap allocations are fast and predictable
- parser/compiler/linker phases can allocate cheaply and release as a group
- long-running tools can return large temporary memory to the system
- vector allocation is overflow-safe by default
- thread-aware tools opt into locking explicitly
- memory behavior is shaped by this repository's needs rather than by legacy libc
  compatibility

## SEE ALSO

runtime, platform, build, testing, compiler, userland