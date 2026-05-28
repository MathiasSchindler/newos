# JQ

## NAME

jq - small JSON selector

## SYNOPSIS

```
jq [-r] FILTER [FILE]
```

## DESCRIPTION

`jq` implements a deliberately small subset of jq-style JSON selection for
dependency-free scripts. Supported filters are:

- `.` - print the full JSON value
- `.key` - print an object field
- `.key.subkey` - print nested object fields

FILE defaults to standard input.

## OPTIONS

- `-r`, `--raw-output` - for string results, print the string content without surrounding quotes.
- `-h`, `--help` - show usage.

## LIMITATIONS

This is not full jq. Arrays, pipes, expressions, updates, iteration, arithmetic,
and complete JSON reformatting are outside the initial scope.

## JSON Output

JSON mode limitation: `jq` does not use the shared JSON Lines envelope for normal output because its
normal output is already JSON (or raw string data with `-r`). Diagnostics and
usage remain plain text in this initial version.

## SEE ALSO

xml2json, json-output
