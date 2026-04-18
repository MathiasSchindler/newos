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

- straightforward argument printing
- shell-friendly use in scripts and pipelines
- newline-terminated output for common command usage

## OPTIONS

The current implementation is intentionally minimal and centers on positional
arguments rather than a large option surface.

## LIMITATIONS

- it does not aim to mirror every host `echo` compatibility quirk
- escape-sequence and flag behavior may be narrower than GNU or BSD variants

## EXAMPLES

```text
echo hello
echo build complete
```

## SEE ALSO

printf, tee, sh
