# PERF

## NAME

perf - sample a command with Linux perf events

## SYNOPSIS

```
perf [-m MAP] [-F HZ] [-n COUNT] [--csv] [--kernel] -- COMMAND [ARG ...]
```

## DESCRIPTION

`perf` runs a command and samples its instruction pointer with Linux
`perf_event_open`. It is the low-overhead companion to `profiler`: `profiler`
needs an instrumentation build and records every function enter/exit, while
`perf` samples normal binaries and reports where CPU time was probably spent.

The first implementation is intentionally small and Linux-focused. It uses the
software `cpu-clock` event, samples user-space instruction pointers by default,
aggregates samples by address, and optionally resolves addresses with linker map
or `nm -n` style symbol files.

For project freestanding binaries, build maps with linker reports and pass the
matching map to `-m`:

```
make freestanding LINKER_REPORTS=1
build/freestanding-linux-x86_64/perf \
  -m build/freestanding-linux-x86_64/.maps/pdfcheck.map \
  -F 997 -n 20 -- \
  build/freestanding-linux-x86_64/pdfcheck file.pdf
```

## OPTIONS

- `-m MAP`, `--map MAP`, `--symbols MAP` - read address-to-function names.
  Accepted formats include simple `address name`, GNU `nm -n`, Mach-O project
  linker `symbol ... __TEXT,__text ...`, and Linux newlinker `.text.NAME` map
  lines.
- `-F HZ`, `--freq HZ` - sampling frequency. The default is 997 samples per
  second. Internally this is expressed as a `cpu-clock` period.
- `-n COUNT`, `--count COUNT` - print at most COUNT rows. The default is 20.
- `--csv` - write CSV output.
- `--kernel` - include kernel samples. The default excludes kernel and hypervisor
  samples so ordinary user-space profiling is less permission-sensitive.
- `--ring-pages COUNT` - set the perf mmap ring size in pages. The default is
  64, which stays below common unprivileged `perf_event_mlock_kb` limits.
- `-h`, `--help` - show usage.

## OUTPUT

Text output starts with a summary line and then the hottest sampled addresses or
functions:

```
samples=1204 lost=0 elapsed_ms=1538 exit=0
rank samples pct address function
1 322 26.74 0x401230 pdf_parse_object
2 217 18.02 0x4059a0 inflate_decode_block
```

`pct` is the row's sample share of all collected samples. Sampling is statistical:
small differences are noise, and short runs may have too few samples to be useful.
For a stable picture, run a workload long enough to collect hundreds or thousands
of samples.

## PERMISSIONS

Linux controls perf access with `kernel.perf_event_paranoid`. If the kernel
rejects `perf_event_open`, this tool exits with status 125 and prints the current
setting when `/proc/sys/kernel/perf_event_paranoid` is readable.

Useful settings while developing locally:

```
sudo sysctl kernel.perf_event_paranoid=1
sudo sysctl kernel.perf_event_paranoid=0
```

`1` is usually enough for user-space CPU-clock sampling. `0` allows broader
profiling, including more kernel and hardware-event use. Some distributions use
`4`, which blocks even ordinary unprivileged perf sampling.

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## LIMITATIONS

- Linux only; non-Linux builds report that `perf_event_open` is required.
- samples only instruction pointers, not call stacks or source lines
- uses the software `cpu-clock` event rather than hardware counters
- starts the target before attaching the event, so very short commands can finish
  before useful samples are collected
- does not currently sample forked child processes; profile the target binary
  directly or use a shell `exec` wrapper when redirecting target output
- symbolization depends on a map or symbol file from the exact binary being run
- PIE/ASLR can shift addresses; project newlinker static outputs are the most
  straightforward targets for symbolized reports
- row limits cap retained unique addresses/functions to keep the tool static and
  dependency-free

## SEE ALSO

profiler, strace, time, top, ps, linker, readelf, objdump
