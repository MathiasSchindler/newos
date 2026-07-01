# STRACE

## NAME

strace - trace system calls

## SYNOPSIS

```
strace [-n] [-p] [-T] [-c] [-o FILE] [-e SYSCALL[,SYSCALL...]] [--json] COMMAND [ARG ...]
strace [-n] [-p] [-T] [-c] [-o FILE] [-e SYSCALL[,SYSCALL...]] [--json] --records FILE
```

## DESCRIPTION

`strace` runs COMMAND under the platform syscall tracing backend and prints one
line for each completed system call. The Linux x86-64 backend uses `ptrace` and
can decode bounded path-like string arguments for common file and process
syscalls, follows forked/cloned children, and decodes selected socket addresses
and `pollfd` arrays. The macOS project-linked backend traces newos freestanding tools by
passing an internal trace pipe to the child and collecting completed events
emitted by selected platform/runtime wrappers.

The output focuses on syscall number/name, the first three arguments, and the
return value. Raw integer or pointer values are printed by default. When the
backend supplies a decoded payload, `strace` prints short strings inline, such
as `open("README.md", O_RDONLY, 0x0)`. Open flags and common negative errno
values are decoded in text output.

## OPTIONS

- `-n`, `--numbers` - include syscall numbers after names.
- `-p`, `--pid` - prefix each text line with the traced process id when the
  backend reports one.
- `-T`, `--time` - append elapsed syscall time in milliseconds when available.
- `-c`, `--summary` - suppress per-call text and print a syscall summary with
  call counts, error counts, byte totals for byte-returning syscalls, average
  returned bytes per call, total time, average time, and maximum observed time.
- `-o FILE`, `--output FILE`, `--output=FILE` - write trace output to FILE
  instead of standard error. The traced command's stdout is unchanged.
- `-e SYSCALL[,SYSCALL...]`, `--trace SYSCALL[,SYSCALL...]` - show only the
  named or numbered syscalls. The `trace=` prefix is accepted, so
  `-e trace=openat,read,write` is equivalent to `-e openat,read,write`.
- `--json` - emit JSON Lines `syscall` events on standard error.
- `--records FILE` - on macOS project-linked builds, replay raw trace records
  captured through `NEWOS_STRACE_FD` instead of launching a command. This is
  intended for stocktake and test-suite aggregation tooling.
- `-h`, `--help` - show usage.

## FILTERS

Examples:

```
strace -e openat,read,write command
strace -e trace=socket,connect,ppoll,write command
strace -e 257,0,1 command
```

Unknown filter names are rejected. Numeric filters use the active platform's
syscall number: Linux x86-64 on the Linux backend and Darwin arm64 on the macOS
project-linked backend.

## UNKNOWN SYSCALLS

If a syscall number is not in the built-in name table, text output uses the
generic name `syscall` and `-n` can be used to show the number:

```
syscall#999(...)
```

JSON output always includes both `"number"` and `"name"`, so consumers can still
distinguish unknown calls and add their own name table. On Linux, decoded string
payloads are captured at syscall-entry stops before `execve` replaces the child
address space.

## POLL AND PPOLL

`poll`, `ppoll`, `select`, and `pselect6` are named. On Linux x86-64, `poll`
and `ppoll` decode a bounded prefix of the pointed-to `pollfd` array:

```
poll([{fd=3,events=POLLIN}], 0x1, 0x0) = 1
```

Long arrays are truncated. `select` and `pselect6` fd-set decoding is still out
of scope.

## SOCKET ADDRESSES

On Linux x86-64, `connect`, `bind`, and `sendto` decode IPv4 and IPv6 socket
addresses where the pointed-to address is readable at syscall entry. Returned
address decoding for calls such as `accept`, `getsockname`, and `getpeername`
is still future work because it requires preserving entry-side pointer state for
exit-side decoding.

## JSON Output

With `--json`, `strace` emits one `syscall` event per completed syscall:

```json
{"schema":"newos.tool.v1","tool":"strace","stream":"stderr","event":"syscall","seq":1,"data":{"number":1,"name":"write","args":[1,4198400,4,0,0,0],"result":4}}
```

Backends that can safely decode one pointed-to argument add a `decoded` object.
Backends that report process and timing metadata add `pid`, `duration_ns`, and
`errno` fields inside `data`:

```json
{"schema":"newos.tool.v1","tool":"strace","stream":"stderr","event":"syscall","seq":1,"data":{"number":4,"name":"write","args":[1,4198400,6,0,0,0],"result":6,"pid":123,"duration_ns":1000}}
```

Diagnostics and usage messages follow the shared `json-output` envelope.

## SUMMARY OUTPUT

With `-c`, text output uses space-separated columns:

```text
syscall calls errors bytes avg_bytes total_ms avg_ms max_ms
read    42    0      8192  195       1.234    0.029  0.120
```

`bytes` and `avg_bytes` are accumulated from non-negative results for byte
returning syscalls such as `read` and `write`. Timing columns use backend event
durations where available. If `-T -c` is requested but the trace stream contains
no non-zero durations, `strace` prints a note that syscall durations are
unavailable in that stream.

## LIMITATIONS

Linux x86-64 reports syscall details through `ptrace`. It decodes selected
NUL-terminated string arguments with bounded child-memory reads, currently for
path-like arguments such as `execve`, `open`, `openat`, `stat`-family calls,
`readlinkat`, and common path-mutating calls. It also follows `fork`, `vfork`,
and `clone` trace events and decodes bounded socket-address and `pollfd` data.
It does not yet decode arbitrary buffers, fd sets, or argument vectors.

macOS arm64 reports syscalls made through selected newos project-linked
platform/runtime wrappers; it does not trace arbitrary system binaries or direct
`svc` instructions outside those wrappers. The macOS backend emits completed
records only, so it is designed for attribution and debugging of project tools
rather than exact kernel-entry emulation.

macOS decoding is intentionally bounded. Path-like string arguments and open
flags are decoded where the wrapper has stable arguments. `read` and `write`
buffer pointers are reported as pointers on the platform-wrapper path instead of
copying potentially unstable in-flight buffers. The macOS backend also avoids
timing syscalls around `read` and `write` records so traced tools can safely
reuse stack I/O buffers after the syscall returns; those calls therefore show a
zero duration placeholder with `-T`. Directory-entry payload decoding is
represented in the trace record format for runtime-originated events, but the
portable text surface should not depend on it yet.

Raw record capture can be filtered with `NEWOS_STRACE_FILTER`. The value `all`
captures all instrumented wrappers. The value `default` captures `open`,
`close`, `stat`, and path-operation records while skipping `read` and `write`,
which is the safer mode for running exact-output test suites.

## FUTURE IMPROVEMENTS

Useful next steps would be fd-set decoding for `select`/`pselect6`, argv/envp
decoding for process syscalls, signal-delivery fidelity, and broader protocol
specific socket option decoding.

## SEE ALSO

ps, lsof, ss, profiler, strace-workflows(7)
