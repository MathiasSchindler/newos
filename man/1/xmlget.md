# XMLGET

## NAME

xmlget - extract XML text or attribute values by selector

## SYNOPSIS

```
xmlget SELECTOR [FILE ...]
xmlget -h
xmlget --help
```

## DESCRIPTION

The `xmlget` tool prints selected text nodes or attribute values. It reads one or more files, or standard input when no file is named or `-` is used.

## CURRENT CAPABILITIES

- select elements with the project path selector syntax
- support final-component attribute and same-name sibling position predicates
- print matching non-blank text nodes, one per line
- print matching attribute values when SELECTOR ends in `/@NAME`
- read one or more files or standard input

## OPTIONS

- `SELECTOR` select elements or attributes using the project selector syntax
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Serialized subtrees are not returned; use `xmlcut` for that.
- Descendant text is not joined into a single element value.
- There is no XPath-style `string()` mode for concatenated descendant text yet.
- Entity references are written as they appear in the XML source.

## EXAMPLES

```
xmlget //title feed.xml
xmlget /root/item/@id file.xml
xmlget //item file.xml
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

xmlcut, xmlquery, xml2lines