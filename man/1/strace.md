# STRACE

## NAME

strace - trace Linux system calls

## SYNOPSIS

```
strace [-n] [--json] COMMAND [ARG ...]
```

## DESCRIPTION

`strace` runs COMMAND under the platform syscall tracing backend and prints one
line for each completed system call. The Linux backend uses `ptrace`; other
platforms currently report that tracing is unsupported.

The initial output focuses on syscall number/name, the first three raw arguments,
and the return value.

## OPTIONS

- `-n`, `--numbers` - include syscall numbers after names.
- `--json` - emit JSON Lines `syscall` events on standard error.
- `-h`, `--help` - show usage.

## JSON Output

With `--json`, `strace` emits one `syscall` event per completed syscall:

```json
{"schema":"newos.tool.v1","tool":"strace","stream":"stderr","event":"syscall","seq":1,"data":{"number":1,"name":"write","args":[1,4198400,4,0,0,0],"result":4}}
```

Diagnostics and usage messages follow the shared `json-output` envelope.

## LIMITATIONS

Only Linux x86-64 currently reports syscall details. Arguments are raw integer
values, not decoded strings or structures. Signal delivery and multi-process
following are not implemented yet.

## SEE ALSO

ps, lsof, ss, profiler
