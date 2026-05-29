# PROFILER

## NAME

profiler - summarize instrumentation profiler traces

## SYNOPSIS

```
profiler [-m SYMBOLS] [-n COUNT] [--sort self|total|calls|addr] [--csv] TRACE
profiler --help-instrumentation
```

## DESCRIPTION

`profiler` is the project-native post-processing tool for a small, useful-enough
instrumentation profiler. It is intentionally narrower than `gprof`: it reads a
function enter/exit trace, reconstructs a call stack, and reports call counts,
self time, total time, average time, and the hottest functions.

The intended compiler path is GCC first and Clang second. Build profiled
programs with compiler instrumentation:

```
gcc   -finstrument-functions -fno-omit-frame-pointer -g -O2 ...
clang -finstrument-functions -fno-omit-frame-pointer -g -O2 ...
```

The project build can add these hooks for GCC/Clang based Linux builds:

```
make freestanding PROFILE=1
NEWOS_PROFILE=cat.nprof build/freestanding-linux-x86_64/cat README.md >/dev/null
profiler cat.nprof
```

When `PROFILE=1` is enabled, the build adds
`-finstrument-functions -fno-omit-frame-pointer -fno-inline` and links the small
Linux platform hook implementation. The runtime is inactive unless
`NEWOS_PROFILE` names an output file. Set `NEWOS_PROFILE=0`, `off`, `false`, or
`no` to force profiling off for a profiled binary.

Runtime hooks emit trace lines in this format:

```
enter TIME_NS ADDRESS
exit  TIME_NS ADDRESS
```

Aliases `e`/`x` and `+`/`-` are also accepted. `TIME_NS` is a monotonic timestamp
in nanoseconds. `ADDRESS` may be decimal, hexadecimal with `0x`, or a full-width
hex address such as those emitted by `nm`.

## OPTIONS

- `-m SYMBOLS`, `--symbols SYMBOLS`, `--map SYMBOLS` - read address-to-function
  names. Accepted symbol formats are:

  ```
  0x401000 function_name
  0000000000401000 T function_name
  ```

- `-n COUNT`, `--limit COUNT` - print only the top COUNT functions. The default
  is 30. Use 0 to print all functions.
- `--sort self` - sort by self time. This is the default.
- `--sort total` - sort by inclusive time.
- `--sort calls` - sort by call count.
- `--sort addr` - sort by function address.
- `--csv` - write machine-readable CSV with nanosecond values.
- `--help-instrumentation` - print the expected GCC/Clang instrumentation flags
  and trace format.
- `-h`, `--help` - show usage.

## OUTPUT

Text output uses tab-separated columns:

```
rank    calls   self_ms total_ms max_ms self% total% avg_self_ms avg_total_ms address function
1       42      12.300  30.100   2.400  40.86 100.00 0.292       0.716        0x401000 parse
```

Definitions:

- `calls` - number of function-entry events.
- `self_ms` - time spent in the function body, excluding child calls.
- `total_ms` - inclusive time from function entry to matching exit.
- `max_ms` - largest inclusive duration observed for one call.
- `self%` and `total%` - percentages relative to the largest inclusive function
  total in the trace, normally `main` for a complete single-threaded trace.
- `avg_self_ms` and `avg_total_ms` - per-call averages.

Diagnostics about malformed lines, unmatched exits, stack overflows, or open
frames are written to standard error.

## EXAMPLES

Create a tiny trace by hand:

```
cat > trace.nprof <<'EOF'
enter 0 0x1000
enter 10 0x2000
exit 30 0x2000
exit 50 0x1000
EOF

cat > symbols.txt <<'EOF'
0x1000 main
0x2000 parse
EOF

profiler -m symbols.txt trace.nprof
profiler --sort total -n 10 -m symbols.txt trace.nprof
profiler --csv -m symbols.txt trace.nprof
```

With GNU `nm` output:

```
nm -n ./program > program.syms
profiler -m program.syms program.nprof
```

Newlinker freestanding binaries are intentionally compact and normally do not
carry a final symbol table, so `nm` may not be able to name their addresses. In
that case, run `profiler` without `-m` for an address-only report, or use symbols
from an equivalent non-stripped GCC/Clang-linked profiling build.

## CURRENT CAPABILITIES

- parses line-oriented GCC/Clang instrumentation traces
- reconstructs nested calls with a fixed-size stack
- computes self time, total time, maximum inclusive call time, call count, and
  averages
- resolves exact or nearest-lower function addresses through simple symbol files
  or `nm -n` output; non-function `nm` symbols are ignored
- writes text or CSV reports
- includes a buffered Linux trace runtime for `PROFILE=1` builds

## LIMITATIONS

- this is an instrumentation trace reporter, not a sampling profiler
- `PROFILE=1` currently targets GCC/Clang based Linux builds; `ncc` is not yet a
  suitable C compiler for this workflow
- the runtime writes buffered text traces; this is much cheaper than flushing
  every event, but still high overhead compared with no profiling
- timings are best used comparatively inside one run, not as exact wall-clock
  runtime measurements
- the trace model is single-threaded: interleaved events from multiple threads
  would require per-thread stack tracking, which is not implemented yet
- instrumentation changes behavior because every instrumented function call runs
  enter/exit hooks; very small or hot functions can be distorted
- profiling builds use `-fno-inline` by default so function-level costs stay
  visible; normal optimized/LTO builds may inline or remove functions that you
  expected to see
- PIE/ASLR can shift addresses. Use symbols from the exact profiled binary, and
  prefer non-PIE/stable-address profiling builds when comparing raw addresses
- compact newlinker outputs usually do not include enough symbol information for
  `nm`; address-only reports still work, and richer newlinker symbol maps remain
  future work
- recursive functions are supported through stack nesting, but address-only
  symbolization cannot distinguish inlined call sites
- async signal profiling, statistical sampling, call graph arc percentages, and
  DWARF source-line attribution are outside the initial scope

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

time, top, ps, linker, readelf, objdump
