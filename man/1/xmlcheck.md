# XMLCHECK

## NAME

xmlcheck - check XML well-formedness

## SYNOPSIS

```
xmlcheck [--stream] [FILE ...]
xmlcheck -h
xmlcheck --help
```

## DESCRIPTION

The `xmlcheck` tool checks whether XML input is well-formed. It reads one or more files, or standard input when no file is named or `-` is used.

## CURRENT CAPABILITIES

- validate element nesting and document-root structure
- detect duplicate attributes on the same element
- detect malformed comments, CDATA sections, processing instructions, DOCTYPE declarations, and entity references
- reject malformed UTF-8 byte sequences and XML-disallowed Unicode code points
- report line and column diagnostics
- reject multiple document roots and non-whitespace text outside the document element
- validate large files with `--stream` without loading the complete document into memory

## OPTIONS

- `--stream` use the streaming well-formedness validator
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- No DTD, XSD, Relax NG, or namespace-aware validation is implemented.
- External entities are not loaded or expanded.
- Declared non-UTF-8 encodings are not transcoded before validation.
- The streaming mode checks well-formedness, not higher-level policy rules.

## EXAMPLES

```
xmlcheck document.xml
xmlcheck --stream large.xml
cat document.xml | xmlcheck
```

## SEE ALSO

xmlvalidate, xmlsafe, xmltokens