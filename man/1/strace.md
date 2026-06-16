# STRACE

## NAME

strace - trace system calls

## SYNOPSIS

```
strace [-n] [-p] [-T] [-c] [-o FILE] [-e SYSCALL[,SYSCALL...]] [--json] COMMAND [ARG ...]
```

## DESCRIPTION

`strace` runs COMMAND under the platform syscall tracing backend and prints one
line for each completed system call. The Linux backend uses `ptrace`. The macOS
project-linked backend traces newos freestanding tools by passing an internal
trace pipe to the child and collecting completed events emitted by selected
platform/runtime wrappers.

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
  call counts, error counts, positive-result byte totals, and total time.
- `-o FILE`, `--output FILE`, `--output=FILE` - write trace output to FILE
  instead of standard error. The traced command's stdout is unchanged.
- `-e SYSCALL[,SYSCALL...]`, `--trace SYSCALL[,SYSCALL...]` - show only the
  named or numbered syscalls. The `trace=` prefix is accepted, so
  `-e trace=openat,read,write` is equivalent to `-e openat,read,write`.
- `--json` - emit JSON Lines `syscall` events on standard error.
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
distinguish unknown calls and add their own name table.

## POLL AND PPOLL

`poll`, `ppoll`, `select`, and `pselect6` are now named. Their first argument is
a pointer to an array or fd-set, so output such as:

```
ppoll(0x7ffd..., 0x1, 0x7ffd...) = 1
```

means "waited on one descriptor and one became ready". Decoding the pointed-to
`pollfd` entries is future work.

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

## LIMITATIONS

Linux x86-64 reports syscall details through `ptrace`. macOS arm64 reports
syscalls made through selected newos project-linked platform/runtime wrappers;
it does not trace arbitrary system binaries or direct `svc` instructions outside
those wrappers. The macOS backend emits completed records only, so it is designed
for attribution and debugging of project tools rather than exact kernel-entry
emulation.

macOS decoding is intentionally bounded. Path-like string arguments and open
flags are decoded where the wrapper has stable arguments. `read` and `write`
buffer pointers are reported as pointers on the platform-wrapper path instead of
copying potentially unstable in-flight buffers. The macOS backend also avoids
timing syscalls around `read` and `write` records so traced tools can safely
reuse stack I/O buffers after the syscall returns; those calls therefore show a
zero duration placeholder with `-T`. Directory-entry payload decoding is
represented in the trace record format for runtime-originated events, but the
portable text surface should not depend on it yet.

## FUTURE IMPROVEMENTS

Useful next steps would be broader argument decoding for socket addresses,
fd-set and `pollfd` decoding for `poll`/`ppoll`, stable child-memory decoding on
Linux, and following cloned child processes.

## SEE ALSO

ps, lsof, ss, profiler
