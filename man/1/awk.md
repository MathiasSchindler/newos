# AWK

## NAME

awk - pattern-action text processing language

## SYNOPSIS

```
awk 'program' [file ...]
```

## DESCRIPTION

`awk` reads input line by line, splits each line into fields, and executes the
program for lines matching its patterns. Programs consist of optional `BEGIN`
and `END` blocks plus pattern-action pairs.

## CURRENT CAPABILITIES

- `BEGIN` and `END` blocks
- Pattern matching with `/regex/` and expression patterns
- Field splitting by `FS` (default: whitespace); `$1`, `$2`, ..., `$NF`
- Built-in variables: `FS`, `OFS`, `RS`, `ORS`, `NR`, `NF`, `FILENAME`
- Arithmetic operators: `+`, `-`, `*`, `/`, `%`, `^`
- String concatenation
- Comparison and logical operators
- `if`/`else`, `while`, `for`, `do`/`while`, `break`, `continue`, `next`,
  `exit`
- Built-in functions: `length`, `substr`, `index`, `split`, `sub`, `gsub`,
  `match`, `sprintf`, `printf`, `print`, `int`, `sqrt`, `sin`, `cos`,
  `atan2`, `exp`, `log`, `rand`, `srand`, `toupper`, `tolower`, `systime`,
  `system`, `getline`
- Arrays (associative)
- Output redirection with `>`, `>>`, `|`

## OPTIONS

Awk takes a single positional program argument followed by optional file
arguments. No `-F`, `-v`, or `-f` flags are currently implemented.

## LIMITATIONS

- `-F` field separator flag is not supported; set `FS` in `BEGIN` instead.
- `-v VAR=VALUE` variable assignment is not supported.
- `-f FILE` script file is not supported; program must be given inline.
- No OFMT/CONVFMT or `ARGC`/`ARGV`.
- `getline` from command pipelines may have limited support.

## EXAMPLES

```
awk '{ print $1 }' file.txt
awk 'BEGIN { FS=":"; OFS="|" } /pattern/ { printf "%s\n", $1 } END { print NR }' file
awk 'NR % 2 == 0' file.txt
awk '{ sum += $1 } END { print sum }' numbers.txt
```

## SEE ALSO

sed, grep, tr
