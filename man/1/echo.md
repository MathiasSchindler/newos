# ECHO

## NAME

echo - write arguments to standard output

## SYNOPSIS

```text
echo [ARG ...]
```

## DESCRIPTION

`echo` prints its arguments separated by spaces and normally terminates the
output with a newline. It is intended for simple shell-friendly text output.

## CURRENT CAPABILITIES

- print arguments separated by single spaces
- always terminate output with a newline
- preserve arguments literally instead of interpreting backslash escapes
- shell-friendly use in scripts and pipelines for simple status output

## OPTIONS

The current implementation is intentionally minimal and centers on positional
arguments rather than a large option surface. Tokens such as `-n`, `-e`, and
`--` are printed as ordinary arguments instead of being treated specially.

## LIMITATIONS

- there is no option to suppress the trailing newline
- backslash escapes are not interpreted; use `printf` when exact formatting or
  portability matters

## EXAMPLES

```text
echo hello
echo build complete
echo "-n is literal here"
```

## SEE ALSO

printf, tee, sh
