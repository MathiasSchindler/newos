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
- Floating-point formatting depends on the project's supported conversion
  subset and may not match libc for every width, precision, or locale case.
- Unicode escape processing is intentionally limited; shell-specific extensions
  should be handled by the caller's shell or another formatter.

## EXAMPLES

```
printf "Hello, %s!\n" world
printf "%d + %d = %d\n" 1 2 3
printf "%08x\n" 255
printf "%-10s %5d\n" item 42
printf "%b" "line1\nline2\n"
printf "%q\n" "can't stop"
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

echo, awk
