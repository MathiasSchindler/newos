# YES

## NAME

yes - repeatedly output a string

## SYNOPSIS

yes [STRING ...]

## DESCRIPTION

yes writes its arguments joined by spaces to standard output, followed by a newline, in an infinite loop. If no arguments are given it writes the single character `y`. The output continues until the process is killed or the write fails.

## CURRENT CAPABILITIES

- infinite output of a user-supplied string
- default `y` output when no arguments are given

## OPTIONS

yes accepts no flags.

## LIMITATIONS

- no built-in output rate limiting
- no way to specify a finite repeat count (use `head -n N` or a shell loop for that)

## EXAMPLES

- `yes` — continuously print `y`
- `yes no` — continuously print `no`
- `yes | head -5` — print `y` five times

## SEE ALSO

seq, echo
