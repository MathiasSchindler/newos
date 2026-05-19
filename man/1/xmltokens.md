# XMLTOKENS

## NAME

xmltokens - print XML parser events

## SYNOPSIS

```
xmltokens [FILE ...]
xmltokens -h
xmltokens --help
```

## DESCRIPTION

The `xmltokens` tool prints the event stream produced by the shared XML parser. It is useful for debugging tools, fixtures, and parser behavior.

## CURRENT CAPABILITIES

- print start, end, empty-element, text, CDATA, comment, processing-instruction, and DOCTYPE events
- show line, column, depth, element names, attributes, and visible text snippets
- read one or more files or standard input

## OPTIONS

- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Output is intended for inspection rather than as a stable machine interchange format.
- No stable `--format=tsv` or `--format=json` parser-event output is implemented yet.
- Entity references are reported as source text rather than decoded values.

## EXAMPLES

```
xmltokens document.xml
xmltokens document.xml | grep 'start depth=0'
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

xmlcheck, xmlcount, xmlstats