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

## EXAMPLES

```
logger system is ready
logger -t init -p notice entered multi-user mode
logger -s -p err service failed
logger -f /tmp/test.log build completed
```

## SEE ALSO

dmesg, init, service
