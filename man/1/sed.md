# SED

## NAME

sed - stream editor for filtering and transforming text

## SYNOPSIS

```
sed [-n] [-i[SUFFIX]] [-e expression] [-f script] [expression] [file ...]
```

## DESCRIPTION

`sed` reads lines from each FILE (or standard input), applies editing commands,
and writes results to standard output. Multiple expressions may be combined.

## CURRENT CAPABILITIES

- Substitution: `s/pattern/replacement/flags`
- Address ranges: line numbers, `$` (last line), regex addresses
- Commands: `s`, `p`, `d`, `q`, `a`, `i`, `c`, `y`, `n`, `N`, `D`, `P`,
  `h`, `H`, `g`, `G`, `x`, `l`, `=`, `r`, `w`, `b`, `t`, `T`, `:` (labels)
- In-place editing with `-i` (optional backup suffix)
- Suppress default output with `-n`
- Multiple expressions with `-e`
- Read script from file with `-f`

## OPTIONS

- `-n` — suppress automatic printing of each line
- `-i[SUFFIX]` — edit files in place; SUFFIX appended to original for backup
- `-e EXPRESSION` — add EXPRESSION to the list of commands
- `-f FILE` — read sed script from FILE

## LIMITATIONS

- No GNU `-z` (NUL-delimited input) option.
- Extended regex (`-E`/`-r`) flag is not supported; patterns use BRE syntax.
- Branch and label support may differ from GNU sed in edge cases.

## EXAMPLES

```
sed 's/foo/bar/g' file.txt
sed -n '/^ERROR/p' log.txt
sed -i.bak 's/old/new/g' config.txt
sed '/^#/d' file.txt
echo "hello" | sed 's/./&\n/g'
```

## SEE ALSO

awk, grep, tr
