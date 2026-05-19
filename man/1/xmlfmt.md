# XMLFMT

## NAME

xmlfmt - format XML with indentation

## SYNOPSIS

```
xmlfmt [-i WIDTH] [FILE ...]
xmlfmt [--indent WIDTH] [FILE ...]
xmlfmt -h
xmlfmt --help
```

## DESCRIPTION

The `xmlfmt` tool writes a simply indented version of XML input. It reads one or more files, or standard input when no file is named or `-` is used.

## CURRENT CAPABILITIES

- remove purely blank text nodes used as existing indentation
- place element structure on separate indented lines
- preserve non-blank text, CDATA, comments, processing instructions, and DOCTYPE tokens
- choose indentation width with `-i` or `--indent`

## OPTIONS

- `-i WIDTH` use WIDTH spaces per indentation level
- `--indent WIDTH` use WIDTH spaces per indentation level
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Formatting is intentionally simple and not a full pretty-printer for every mixed-content style.
- Attributes, namespaces, entity spelling, and text content are not canonicalized.
- The tool is document-buffered and needs the complete input to fit in memory.

## EXAMPLES

```
xmlfmt document.xml
xmlfmt -i 4 document.xml
xmlfmt document.xml | xmlcheck
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

xmlmin, xmlcanon, xmlcheck