# XARGS

## NAME

xargs - build and execute command lines from standard input

## SYNOPSIS

```
xargs [-0] [-r] [-t] [-d DELIM] [-n MAXARGS] [-s MAXCHARS]
      [-P MAXPROCS] [-I REPLSTR] [command [initial-args ...]]
```

## DESCRIPTION

`xargs` reads items from standard input (whitespace or NUL-delimited) and
passes them as arguments to COMMAND. If no COMMAND is given, `echo` is used.

## CURRENT CAPABILITIES

- Batch arguments up to MAXARGS items per invocation with `-n`
- Limit total argument-string length with `-s`
- Run up to MAXPROCS invocations in parallel with `-P`
- Replace a placeholder string in arguments with `-I REPLSTR` (one item per
  invocation, reads by line)
- Accept NUL-delimited input with `-0`
- Accept custom single-character delimiters with `-d`
- Skip invoking the command on empty input with `-r`
- Trace each command before running it with `-t`
- Use `echo` as the default command when none is specified

## OPTIONS

- `-0` — expect NUL-delimited input instead of whitespace-delimited
- `-r` — do not run the command if no input items were read
- `-t` — print each command line to stderr before executing it
- `-d DELIM` — split input on DELIM (supports `\n`, `\r`, `\t`, `\0`, and `\\`)
- `-n MAXARGS` — use at most MAXARGS arguments per command invocation
- `-s MAXCHARS` — limit total command-line length to MAXCHARS bytes
- `-P MAXPROCS` — run up to MAXPROCS invocations concurrently
- `-I REPLSTR` — replace REPLSTR in initial-args with each input line; implies
  one item per invocation

## LIMITATIONS

- No interactive prompting (`-p`) support.
- `xargs` does not invoke a shell unless you explicitly run one, so wildcard
  expansion, redirection, and pipelines are not interpreted automatically.

## EXAMPLES

```
find . -name "*.o" | xargs rm
printf 'a,b,c\n' | xargs -d , -n 2 echo
find . -name "*.c" | xargs -n 4 gcc -c
find . -name "*.txt" -print0 | xargs -0 grep -l "TODO"
ls *.log | xargs -I{} cp {} backup/
```

## SEE ALSO

find, sh
