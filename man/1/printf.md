# PRINTF

## NAME

printf - format and print data

## SYNOPSIS

```
printf FORMAT [ARG ...]
```

## DESCRIPTION

`printf` formats the ARG values according to FORMAT and writes the result to
standard output. FORMAT follows C `printf` conventions.

## CURRENT CAPABILITIES

- Conversion specifiers: `%d`, `%i`, `%u`, `%o`, `%x`, `%X`, `%f`, `%e`,
  `%E`, `%g`, `%G`, `%s`, `%c`, `%b`, `%q`, `%%`
- Escape sequences in FORMAT: `\n`, `\t`, `\r`, `\\`, `\a`, `\b`, `\f`,
  `\v`, octal `\NNN`, hex `\xNN`
- Width and precision fields
- Flags: `-` (left-align), `+` (force sign), ` ` (space for positive),
  `0` (zero-pad), `#` (alternate form)
- FORMAT repeats until all supplied arguments are consumed

## OPTIONS

None. FORMAT is always the first argument.

## LIMITATIONS

- `%q` uses simple POSIX shell single-quote escaping; it does not attempt locale-specific or shell-specific `$'...'` forms.
- No `-v VAR` assignment mode.

## EXAMPLES

```
printf "Hello, %s!\n" world
printf "%d + %d = %d\n" 1 2 3
printf "%08x\n" 255
printf "%-10s %5d\n" item 42
printf "%b" "line1\nline2\n"
printf "%q\n" "can't stop"
```

## SEE ALSO

echo, awk
