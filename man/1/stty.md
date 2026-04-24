# STTY

## NAME

stty - inspect or change a terminal mode subset

## SYNOPSIS

```sh
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

```sh
stty -a
stty size
stty raw -echo
stty sane
stty rows 40 cols 120
```

## SEE ALSO

getty, login, sh
