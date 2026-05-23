# BASE64

## NAME

base64 - encode or decode RFC 4648 base64 data

## SYNOPSIS

```
base64 [-d] [-w COLS] [FILE]
```

## DESCRIPTION

`base64` encodes FILE, or standard input, to base64. With `-d` it decodes base64
input back to bytes.

## OPTIONS

- `-d`, `--decode` - decode input.
- `-w COLS` - wrap encoded output after COLS columns. Use `-w 0` to disable wrapping.
- `-h`, `--help` - show usage.

## JSON Output

`base64` does not implement `--json` because its primary output is encoded or
decoded byte data. Diagnostics and usage remain plain text.

## SEE ALSO

cat, hexdump, od
