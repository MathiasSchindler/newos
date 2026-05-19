# LOGGER

## NAME

logger - write messages to the system log stream

## SYNOPSIS

```
logger [-s] [-t TAG] [-p PRIORITY] [-f FILE] [MESSAGE...]
```

## DESCRIPTION

`logger` writes a message to `/dev/kmsg` when available, or to standard error as
a fallback. With `-f`, it appends to a chosen file instead. If no message
operands are supplied, standard input is logged one line at a time.

Messages are formatted as `TAG[PID]: MESSAGE`; kernel-message writes also include
a Linux severity prefix such as `<6>`.

## OPTIONS

- `-s` - also mirror messages to standard error
- `-t TAG` - set the tag; the default is `logger`
- `-p PRIORITY` - set severity as `0` through `7`, a level name such as `err`,
  `notice`, or `debug`, or a `facility.level` string
- `-f FILE` - append log records to `FILE` instead of `/dev/kmsg`
- `-h`, `--help` - show usage information

## LIMITATIONS

- Writes to `/dev/kmsg` or a chosen file; it does not speak syslog over UDP,
  TCP, TLS, or a Unix-domain `/dev/log` socket.
- Facility names in `-p facility.level` are accepted for familiar syntax, but
  output still maps to the simple severity prefix used by the current backend.
- No structured logging fields, RFC 5424 formatting, journald integration, or
  remote log forwarding are implemented yet.
- Log rotation, rate limiting, and durable queueing are outside this tool's
  current scope.

## EXAMPLES

```
logger system is ready
logger -t init -p notice entered multi-user mode
logger -s -p err service failed
logger -f /tmp/test.log build completed
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

dmesg, init, service
