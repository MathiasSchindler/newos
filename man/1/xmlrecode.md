# XMLRECODE

## NAME

xmlrecode - convert supported XML byte encodings to UTF-8

## SYNOPSIS

```
xmlrecode [--from ENCODING] [FILE ...]
xmlrecode -h
xmlrecode --help
```

## DESCRIPTION

The `xmlrecode` tool converts supported legacy XML byte encodings to UTF-8 so the result can be passed to the rest of the XML tool pipeline.

When `--from` is omitted, `xmlrecode` looks for an XML declaration at the start of the input and uses its `encoding` value when it is one of the supported encodings. If no declaration is found, input is treated as UTF-8 and validated.

For converted inputs with an XML declaration, the declaration is rewritten as `encoding="UTF-8"`.

## CURRENT CAPABILITIES

- pass through valid UTF-8 input
- convert ISO-8859-1 / Latin-1 to UTF-8
- convert Windows-1252 / CP1252 to UTF-8
- detect supported encodings from an initial XML declaration
- rewrite the XML declaration encoding to UTF-8 when conversion occurs

## OPTIONS

- `--from ENCODING` force the input encoding; supported values are `utf-8`, `iso-8859-1`, `latin1`, `windows-1252`, and `cp1252`
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- This is a small recoder, not a full iconv replacement.
- UTF-16, UTF-32, Shift_JIS, EUC-JP, and other encodings are not implemented yet.
- Only a declaration at the start of the input is inspected.
- Converted output is not pretty-printed or otherwise normalized.

## EXAMPLES

```
xmlrecode legacy.xml | xmlcheck
xmlrecode --from windows-1252 feed.xml > feed.utf8.xml
```

## SEE ALSO

xmlcheck, xmlvalidate, xml2yaml