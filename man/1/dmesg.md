# DMESG

## NAME

dmesg - inspect the kernel message buffer

## SYNOPSIS

```sh
dmesg
dmesg [-crwC] [-l LEVELS] [-n LEVEL]
```

## DESCRIPTION

`dmesg` prints messages from the kernel log buffer. In its default mode it reads
the current buffered log and formats Linux-style records into a readable stream.
It can also follow new messages, clear the buffer, or adjust the console log
level when the host platform exposes those kernel interfaces.

This is a compact Linux-first implementation intended for diagnostics, boot
inspection, and early userspace debugging.

## CURRENT CAPABILITIES

- display the current kernel message buffer
- follow new log messages with `-w`
- keep raw kernel record prefixes with `-r`
- filter Linux severity levels with `-l`
- clear the buffer with `-C` or read-and-clear with `-c` where supported
- adjust the console log level with `-n`

## OPTIONS

- `-r`, `--raw` - keep the log records closer to their kernel-provided form
- `-w`, `--follow` - wait for and print new kernel log messages as they arrive
- `-l LEVELS`, `--level LEVELS` - show only the selected comma-separated levels;
  names such as `err`, `warn`, `info`, and `debug` are accepted, as are numeric
  levels `0` through `7`
- `-c`, `--read-clear` - print the current buffer and then clear it when the
  platform supports that operation
- `-C`, `--clear` - clear the current kernel log buffer without printing it
- `-n LEVEL`, `--console-level LEVEL` - ask the kernel to change the console log
  level
- `-h`, `--help` - show usage information

## LIMITATIONS

- live follow mode depends on Linux-style `/dev/kmsg` or `/proc/kmsg`
- reading or clearing the kernel log may require elevated privileges, depending
  on kernel policy
- non-Linux hosted platforms may only expose a boot log snapshot, or no kernel
  log at all
- no colorization, pager integration, human timestamp conversion, or JSON output
  yet

## EXAMPLES

```sh
dmesg
dmesg -l err,warn
dmesg -w
dmesg -c
dmesg -n notice
```

## SEE ALSO

man, uptime, init, getty
