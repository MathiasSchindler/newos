# TR

## NAME

tr - translate or delete characters

## SYNOPSIS

```
tr [-c] [-d] [-s] SET1 [SET2]
```

## DESCRIPTION

`tr` reads from standard input, applies character translation, deletion, or
squeeze operations, and writes to standard output.

## CURRENT CAPABILITIES

- Translate characters in SET1 to corresponding characters in SET2
- Delete characters in SET1 with `-d`
- Squeeze repeated characters in SET1 with `-s`
- Complement SET1 with `-c`
- POSIX character classes: `[:alpha:]`, `[:digit:]`, `[:lower:]`, `[:upper:]`,
  `[:alnum:]`, `[:space:]`, `[:blank:]`, `[:xdigit:]`, `[:cntrl:]`, `[:print:]`,
  `[:graph:]`, `[:punct:]`
- Ranges (e.g. `a-z`), octal escapes (`\000`–`\377`), hex escapes (`\xNN`),
  and named escapes (`\n`, `\t`, `\r`, etc.)

## OPTIONS

- `-c` — complement SET1 (operate on characters not in SET1)
- `-d` — delete characters in SET1; SET2 not used
- `-s` — squeeze consecutive repeated characters in the resulting output

## LIMITATIONS

- No repeat (`[c*n]`) notation.
- No equivalence classes (`[=c=]`).
- Input is treated as bytes; multi-byte/wide-character encodings are not handled.
- SET capacity is 512 characters.

## EXAMPLES

```
echo "Hello World" | tr 'a-z' 'A-Z'
echo "hello   world" | tr -s ' '
echo "abc123" | tr -d '0-9'
cat file | tr -cd '[:print:]\n'
```

## SEE ALSO

sed, awk
