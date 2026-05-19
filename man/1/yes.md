# YES

## NAME

yes - repeatedly output a string

## SYNOPSIS

```
yes [STRING ...]
```

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
- output continues until the pipe or terminal write fails; there is no
  interactive stop condition inside the tool
- no NUL-delimited or random-content mode is implemented

## EXAMPLES

- `yes` — continuously print `y`
- `yes no` — continuously print `no`
- `yes | head -5` — print `y` five times

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

seq, echo
