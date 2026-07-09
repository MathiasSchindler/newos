# LSUSB

## NAME

lsusb - list USB devices

## SYNOPSIS

```
lsusb [-v] [-t] [-p] [-a] [-r] [-d VID[:PID]] [-c CLASS]
	[-s BUS:DEVICE] [-D PATH] [--json]
```

## DESCRIPTION

`lsusb` lists USB devices discovered through the platform USB backend. The
compact output is sorted by bus and device address and shows bus number, device
address, vendor/product ID, device class, configuration count, and available
manufacturer and product strings. Linux sysfs metadata enriches verbose output
with topology, speed, USB/device versions, serial number, active configuration,
authorization state, and the device-level driver.

On macOS freestanding builds, the tool uses the raw Mach/IOKit USB backend. On
Linux freestanding builds, it uses `/dev/bus/usb`. Device discovery and
descriptor inspection read the usbfs descriptor stream without requiring
write access to device nodes; interface claims and live transfers use usbfs
ioctls when the caller has permission.

## CURRENT CAPABILITIES

- Compact device listing
- Bus-oriented tree view with `-t`
- Descriptor detail with `-v`
- Contextual names for common standard and class-specific descriptors
- Vendor/product filtering with `-d`
- Device-class filtering with `-c`
- Backend path display with `-p`
- Bus/device and exact backend-path selection
- Raw configuration descriptor dumps with truncation reporting
- JSON Lines device, configuration, interface, and endpoint events

## OPTIONS

- `-v`, `--verbose` — read and print configuration, interface, and endpoint descriptors
- `-t`, `--tree` — group output by USB bus
- `-p`, `--path` — include the backend path used to reopen the device
- `-a`, `--all` — include explanatory rows when verbose descriptor reads fail
- `-r`, `--raw` — enable verbose output and dump raw configuration descriptors in hexadecimal
- `-d VID[:PID]`, `--device VID[:PID]` — filter by hexadecimal vendor ID and optional product ID
- `-c CLASS`, `--class CLASS` — filter by hexadecimal USB device class
- `-s BUS:DEVICE`, `--select BUS:DEVICE` — select one decimal bus and device address
- `-D PATH`, `--device-path PATH` — select one exact backend path
- `--json` — emit JSON Lines; combine with `-v` for descriptor hierarchy events
- `-h`, `--help` — show usage

## EXAMPLES

```
lsusb
lsusb -t
lsusb -v -p
lsusb -d 05ac
lsusb -d 05ac:12a8
lsusb -c 09
lsusb -s 1:4 -v
lsusb -D /dev/bus/usb/001/004 -r
lsusb --json -v
```

## LIMITATIONS

- Names come from device/sysfs strings; IDs are not resolved through `usb.ids`.
- Linux topology is the sysfs USB node name. The tree view remains bus-oriented
	rather than reconstructing the full parent/port hierarchy.
- Linux interface claims and control/bulk transfers require write access to the
	usbfs device node and can be blocked while a kernel driver owns the interface.
- macOS live transfers can still be blocked by another driver owning an interface.
- Windows currently links the API-complete stub backend.

## JSON Output

`--json` emits one `device` event per selected device. With `-v`, each readable
configuration emits a `configuration` event followed by its `interface` and
`endpoint` events. Every line uses the `newos.tool.v1` envelope. USB IDs and
descriptor values are JSON numbers; sysfs-provided versions and speed are
strings so their source spelling is preserved. See `json-output` for common
envelope and compatibility rules.

## SEE ALSO

usbmon, lsof, df, system_profiler
