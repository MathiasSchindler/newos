# XMLVALIDATE

## NAME

xmlvalidate - check XML well-formedness and simple structural policy

## SYNOPSIS

```
xmlvalidate [--stream] [--dtd FILE|auto] [--allow-doctype] [--allow-pi]
  [--allow-comments] [--max-depth N] [--root NAME] [FILE ...]
xmlvalidate -h
xmlvalidate --help
```

## DESCRIPTION

The `xmlvalidate` tool checks XML well-formedness and can enforce a small set of structural rules. It reads one or more files, or standard input when no file is named or `-` is used.

## CURRENT CAPABILITIES

- perform the same document-buffered well-formedness parsing as the XML tools
- reject malformed UTF-8 byte sequences and XML-disallowed Unicode code points
- require a specific document root with `--root NAME`
- enforce a maximum element depth with `--max-depth N`
- reject DOCTYPE declarations, processing instructions, and comments unless allowed by option
- use streaming validation for large inputs with `--stream`
- enforce the same structural policy options in streaming and buffered modes
- validate a supported DTD subset with `--dtd FILE` or `--dtd auto`
- enforce DTD root name, declared elements, `EMPTY` elements, required attributes, fixed attributes, unique IDs, and IDREF/IDREFS resolution

## OPTIONS

- `--stream` use the streaming validator
- `--dtd FILE|auto` validate against declarations in FILE, or against the document DOCTYPE when `auto` is used
- `--allow-doctype` allow DOCTYPE declarations
- `--allow-pi` allow processing instructions
- `--allow-comments` allow comments
- `--max-depth N` reject documents deeper than N elements
- `--root NAME` require the document root to have NAME
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Streaming mode reports policy failures with stream positions rather than parser token positions.
- `--dtd` uses buffered validation even when `--stream` is also supplied.
- No declarative policy file format is implemented yet for rules such as required children, forbidden elements, or per-element constraints.
- DTD support is intentionally partial: full content-model validation, parameter entities, notations, conditional sections, and entity expansion are not implemented.
- External DTD subsets are loaded only when passed explicitly with `--dtd FILE`; network loading is not implemented.
- XSD, Relax NG, and namespace-aware validation are not implemented.
- Declared non-UTF-8 encodings are not transcoded before validation.

## EXAMPLES

```
xmlvalidate --root feed input.xml
xmlvalidate --max-depth 32 input.xml
xmlvalidate --stream --root feed --max-depth 32 large.xml
xmlvalidate --dtd auto input.xml
xmlvalidate --dtd schema.dtd input.xml
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

xmlcheck, xmlsafe, xmlnscheck, xmldtdapply, xmldtdinfo