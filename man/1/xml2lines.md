# XML2LINES

## NAME

xml2lines - flatten XML into path/value lines

## SYNOPSIS

```
xml2lines [FILE ...]
xml2lines -h
xml2lines --help
```

## DESCRIPTION

The `xml2lines` tool writes XML attributes and text-bearing nodes as path/value lines. It is useful for quick inspection, diffs, and simple text pipelines.

## CURRENT CAPABILITIES

- emit one line for each attribute
- emit one line for each non-blank text or CDATA token
- emit an empty value for empty elements
- escape backslashes, newlines, carriage returns, tabs, and `=` in values so each result remains one line

## OPTIONS

- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Value escaping is a simple backslash convention for line-oriented output, not JSON, XML, or shell escaping.
- Descendant text is not combined into a single element value.
- Repeated elements naturally produce repeated paths.

## EXAMPLES

```
xml2lines document.xml
xml2lines document.xml | grep /title
```

## SEE ALSO

xmlget, xmlgrep, xmlcount