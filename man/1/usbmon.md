# USBMON

## NAME

usbmon - monitor and decode Linux USB traffic

## SYNOPSIS

```
usbmon [-i FILE] [-b BUS] [-d DEVICE] [-e ENDPOINT] [-t TYPE]
       [-n COUNT] [-x] [-r] [--json]
```

## DESCRIPTION

`usbmon` reads the Linux usbmon text ABI and prints bounded, decoded transfer
records. By default it opens `/sys/kernel/debug/usb/usbmon/0u`, the text stream
covering all buses. `--input` reads a saved stream or standard input instead,
which is useful for replay, testing, and non-Linux analysis.

Each decoded record includes timestamp, kernel tag, submit/complete/error event,
transfer type, direction, bus/device/endpoint address, status, requested or
completed length, and control setup fields where present. Captured payload bytes
are hidden unless `--data` is requested and are bounded to 512 hexadecimal
characters per record. Isochronous records include their total frame descriptor
count and up to the five descriptors exposed by the kernel text ABI.

## OPTIONS

- `-i FILE`, `--input FILE` — read text records from `FILE`; use `-` for standard input
- `-b BUS`, `--bus BUS` — select a decimal bus number
- `-d DEVICE`, `--device DEVICE` — select a decimal device address
- `-e ENDPOINT`, `--endpoint ENDPOINT` — select a decimal endpoint number
- `-t TYPE`, `--type TYPE` — select `control`, `bulk`, `interrupt`, or `isochronous`
- `-n COUNT`, `--count COUNT` — stop after `COUNT` matching records
- `-x`, `--data` — include captured payload bytes
- `-r`, `--raw` — print matching source records unchanged
- `--json` — emit JSON Lines transfer events
- `-h`, `--help` — show usage

## EXAMPLES

```
usbmon
usbmon -b 1 -d 4 -x
usbmon -t bulk -n 20
usbmon --input capture.txt --json --data
cat capture.txt | usbmon --input - --raw -b 2
```

## CAPTURE SETUP

The live source requires Linux usbmon and readable debugfs files. A typical
administrator setup mounts debugfs at `/sys/kernel/debug` and loads the
`usbmon` module. Access should be granted narrowly because USB traffic may
contain credentials, input events, or application data. `usbmon` reports an
actionable error when the default source cannot be opened.

## INPUT FORMAT

The parser accepts the documented usbmon `0u` text format: tag, timestamp,
event, transfer address, compound status, control setup packet or isochronous
frame descriptors, length, and optional captured data. Input lines are limited
to 4095 bytes. Malformed or oversized
records are diagnosed and cause a nonzero exit status; monitoring continues
past malformed complete lines where possible.

The binary usbmon mmap ABI is intentionally not consumed because its native
record layout is architecture-sensitive. Text captures remain portable across
host and freestanding builds.

## JSON Output

Each matching record is a `transfer` event in the `newos.tool.v1` envelope.
The data object contains the decoded event, address, status, length, optional
five-word control setup array, optional payload, and a payload truncation flag.

## SEE ALSO

lsusb, strace, dmesg