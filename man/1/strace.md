# STRACE

## NAME

strace - trace Linux system calls

## SYNOPSIS

```
strace [-n] [-e SYSCALL[,SYSCALL...]] [--json] COMMAND [ARG ...]
```

## DESCRIPTION

`strace` runs COMMAND under the platform syscall tracing backend and prints one
line for each completed system call. The Linux backend uses `ptrace`. The macOS
project-linked backend traces newos freestanding tools by passing an internal
trace pipe to the child and collecting events emitted by the in-tree Darwin
syscall wrappers.

The initial output focuses on syscall number/name, the first three raw arguments,
and the return value. Arguments are printed as raw integer or pointer values; the
tool does not yet decode pointed-to structures such as `struct pollfd`.

## OPTIONS

- `-n`, `--numbers` - include syscall numbers after names.
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

Diagnostics and usage messages follow the shared `json-output` envelope.

## LIMITATIONS

Linux x86-64 reports syscall details through `ptrace`. macOS arm64 reports
syscalls made through the newos project-linked Darwin syscall wrappers; it does
not trace arbitrary system binaries or direct `svc` instructions outside those
wrappers. Arguments are raw integer values, not decoded strings or structures.
Signal delivery and structured multi-process attribution are not implemented yet.

## FUTURE IMPROVEMENTS

Useful next steps would be string/path decoding for calls such as `openat` and
`execve`, fd-set and `pollfd` decoding for `poll`/`ppoll`, elapsed time per
syscall, summary counts, and following cloned child processes.

## SEE ALSO

ps, lsof, ss, profiler
