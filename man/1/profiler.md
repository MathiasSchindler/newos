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

Runtime hooks should emit trace lines in this format:

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
rank    calls   self_ms total_ms avg_self_ms avg_total_ms address function
1       42      12.300  30.100   0.292       0.716        0x401000 parse
```

Definitions:

- `calls` - number of function-entry events.
- `self_ms` - time spent in the function body, excluding child calls.
- `total_ms` - inclusive time from function entry to matching exit.
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

## CURRENT CAPABILITIES

- parses line-oriented GCC/Clang instrumentation traces
- reconstructs nested calls with a fixed-size stack
- computes self time, total time, maximum inclusive call time, call count, and
  averages
- resolves exact or nearest-lower addresses through simple symbol files or
  `nm -n` output
- writes text or CSV reports

## LIMITATIONS

- this is an instrumentation trace reporter, not a sampling profiler
- runtime hook emission is intentionally separate from this report tool
- recursive functions are supported through stack nesting, but address-only
  symbolization cannot distinguish inlined call sites
- async signal profiling, statistical sampling, call graph arc percentages, and
  DWARF source-line attribution are outside the initial scope

## SEE ALSO

time, top, ps, linker, readelf, objdump
