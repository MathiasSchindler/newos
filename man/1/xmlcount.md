# XMLCOUNT

## NAME

xmlcount - count basic XML structures

## SYNOPSIS

```
xmlcount [FILE ...]
xmlcount -h
xmlcount --help
```

## DESCRIPTION

The `xmlcount` tool counts basic structures in XML input and prints one statistic per line. It reads one or more files, or standard input when no file is named or `-` is used.

## CURRENT CAPABILITIES

- count elements, attributes, non-blank text nodes, comments, CDATA sections, processing instructions, and DOCTYPE declarations
- report maximum element depth
- read one or more files or standard input

## OPTIONS

- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Per-element histograms are not produced; use `xmlstats` for name counts.
- CDATA with content is counted as both `cdata` and a text-bearing node.

## EXAMPLES

```
xmlcount document.xml
xmlcount *.xml
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

xmlstats, xmltokens, xmlcheck