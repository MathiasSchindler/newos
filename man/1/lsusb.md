# LSUSB

## NAME

lsusb - list USB devices

## SYNOPSIS

```
lsusb [-v] [-t] [-p] [-a] [-d VID[:PID]] [-c CLASS]
```

## DESCRIPTION

`lsusb` lists USB devices discovered through the platform USB backend. The
compact output is sorted by bus and device address and shows bus number, device
address, vendor/product ID, device class, configuration count, and optionally
the backend path.

On macOS freestanding builds, the tool uses the raw Mach/IOKit USB backend. On
Linux freestanding builds, it uses `/dev/bus/usb` and `usbfs` ioctls.

## CURRENT CAPABILITIES

- Compact device listing
- Bus-oriented tree view with `-t`
- Descriptor detail with `-v`
- Contextual names for common standard and class-specific descriptors
- Vendor/product filtering with `-d`
- Device-class filtering with `-c`
- Backend path display with `-p`

## OPTIONS

- `-v`, `--verbose` — read and print configuration, interface, and endpoint descriptors
- `-t`, `--tree` — group output by USB bus
- `-p`, `--path` — include the backend path used to reopen the device
- `-a`, `--all` — include explanatory rows when verbose descriptor reads fail
- `-d VID[:PID]`, `--device VID[:PID]` — filter by hexadecimal vendor ID and optional product ID
- `-c CLASS`, `--class CLASS` — filter by hexadecimal USB device class
- `-h`, `--help` — show usage

## EXAMPLES

```
lsusb
lsusb -t
lsusb -v -p
lsusb -d 05ac
lsusb -d 05ac:12a8
lsusb -c 09
```

## LIMITATIONS

- Device names are not resolved through `usb.ids`.
- Host-controller topology is inferred from bus grouping; full port topology is not yet modeled.
- macOS live transfers can still be blocked by another driver owning an interface.
- Windows currently links the API-complete stub backend.

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

lsof, df, system_profiler
