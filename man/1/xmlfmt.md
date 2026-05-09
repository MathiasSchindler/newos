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

## SEE ALSO

xmlmin, xmlcanon, xmlcheck