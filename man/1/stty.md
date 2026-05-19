# STTY

## NAME

stty - inspect or change a terminal mode subset

## SYNOPSIS

```
stty [-a]
stty size
stty raw|sane|echo|-echo|icanon|-icanon|isig|-isig|ixon|-ixon|opost|-opost ...
stty rows N
stty cols N
```

## DESCRIPTION

`stty` reads and updates terminal settings on standard input. This implementation
provides the small mode subset used by the project and by early userspace scripts:
echo, canonical input, signal characters, software flow control, output
processing, and terminal row/column size.

## OPTIONS AND MODES

- `-a` - print the supported modes and terminal size
- `size` - print `ROWS COLUMNS`
- `raw` - disable echo, canonical input, signal characters, flow control, and
  output processing
- `sane` or `cooked` - enable the supported interactive defaults
- `echo`, `-echo` - enable or disable input echo
- `icanon`, `-icanon` - enable or disable canonical line input
- `isig`, `-isig` - enable or disable terminal signal characters
- `ixon`, `-ixon` - enable or disable software flow control
- `opost`, `-opost` - enable or disable output processing
- `rows N`, `cols N`, `columns N` - set terminal dimensions where supported

## EXAMPLES

```
stty -a
stty size
stty raw -echo
stty sane
stty rows 40 cols 120
```

## LIMITATIONS

- Only the listed mode names are understood; common flags such as `intr`,
  `erase`, `kill`, `parenb`, `cs8`, baud-rate changes, and `-g` snapshots are
  not implemented.
- Operates on standard input only; there is no `-F DEVICE`/`-f DEVICE` option
  to inspect or change another terminal.
- Terminal-size updates depend on platform support and may be rejected for
  pseudo-terminals or hosted backends that do not expose `TIOCSWINSZ`.
- Output is compact and project-specific rather than byte-for-byte compatible
  with GNU, BSD, or BusyBox `stty -a`.

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

getty, login, sh
