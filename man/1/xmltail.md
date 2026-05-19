# XMLTAIL

## NAME

xmltail - print the last matching XML element subtrees

## SYNOPSIS

```
xmltail [-n COUNT] [--wrap NAME] SELECTOR [FILE ...]
xmltail -h
xmltail --help
```

## DESCRIPTION

The `xmltail` tool prints the last COUNT selected element subtrees. The default count is 10. By default the output is a newline-delimited XML fragment stream; use `--wrap NAME` to synthesize one wrapper element around all selected subtrees.

## CURRENT CAPABILITIES

- select element subtrees with the project selector syntax
- support final-component attribute and same-name sibling position predicates
- retain the last COUNT matches in a ring buffer
- grow the ring buffer to the requested count
- optionally wrap all matches in a single element with `--wrap NAME`
- read one or more files or standard input

## OPTIONS

- `-n COUNT` print the last COUNT selected subtrees
- `--count COUNT` print the last COUNT selected subtrees
- `--wrap NAME` emit `<NAME>` before matches and `</NAME>` after matches
- `SELECTOR` select element subtrees using the project selector syntax
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Only element subtrees are printed.
- Without `--wrap`, multiple matches are XML fragments, not one well-formed document.
- Large COUNT values use proportionally more in-process memory.
- Ancestor namespace declarations are not added when they live outside the selected subtree.

## EXAMPLES

```
xmltail -n 5 //entry feed.xml
xmltail -n 5 --wrap results //entry feed.xml
xmltail //item document.xml
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

xmlhead, xmlcut, xmlsplit