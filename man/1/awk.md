# AWK

## NAME

awk - pattern-action text processing language

## SYNOPSIS

```
awk [-F sep] [-v var=value] [-f program-file]... ['program'] [file ...]
```

## DESCRIPTION

`awk` reads input records, splits them into fields, and executes pattern-action
rules. This implementation focuses on a practical, low-dependency subset that
covers common field-processing tasks without pulling in a heavyweight runtime.

## CURRENT CAPABILITIES

- `BEGIN` and `END` blocks
- Regex pattern matching with `/regex/` and `expr ~ /regex/`
- Record and field access via `$0`, `$1` ... `$NF`
- Built-in variables: `FS`, `OFS`, `RS`, `ORS`, `NR`, `FNR`, `NF`, `FILENAME`
- Simple variable assignment in the program and via `-v var=value`
- `print` and `printf`
- Inline programs or one/more `-f` program files

## OPTIONS

| Flag | Description |
|------|-------------|
| `-F sep` | Set `FS` before executing `BEGIN` blocks. Common escapes like `\t` are accepted. |
| `-v var=value` | Predefine a variable or separator setting before execution. |
| `-f program-file` | Read program text from `program-file`. May be used more than once. |

## LIMITATIONS

- This is still a focused awk subset rather than full POSIX/GNU awk.
- No user-defined functions, range patterns, `ARGC`/`ARGV`, or the full awk
  expression/control-flow language are implemented.
- `RS` is treated as a literal record separator string rather than full awk
  regex/paragraph-mode semantics.
- No `OFMT`/`CONVFMT`, locale-aware formatting, or advanced `getline` forms.

## EXAMPLES

```
awk '{ print $1 }' file.txt
awk -F: '{ print $1 }' /etc/passwd
awk -v prefix=tag 'FNR == 1 { print FILENAME } { print prefix, $1 }' file1 file2
awk -f script.awk input.txt
```

## SEE ALSO

sed, grep, tr
