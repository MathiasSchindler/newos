# XMLCHECK

## NAME

xmlcheck - check XML well-formedness

## SYNOPSIS

```
xmlcheck [--stream] [--buffered] [FILE ...]
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
- buffered mode transcodes UTF-16 BOM input and declaration-selected ISO-8859-1 or Windows-1252 input to internal UTF-8
- report line and column diagnostics
- reject multiple document roots and non-whitespace text outside the document element
- validate large files without loading the complete document into memory

## OPTIONS

- `--stream` use the streaming well-formedness validator; this is the default
- `--buffered` use the document-buffered parser, mainly for diagnostics comparison
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- No DTD, XSD, Relax NG, or namespace-aware validation is implemented.
- External entities are not loaded or expanded.
- Streaming mode accepts UTF-8 only; focused transcoding is currently provided by the shared buffered reader used by buffered mode and the broader XML command family.
- `--buffered` loads the complete document into memory.

## EXAMPLES

```
xmlcheck document.xml
xmlcheck --stream large.xml
xmlcheck --buffered document.xml
cat document.xml | xmlcheck
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

xmlvalidate, xmlsafe, xmltokens