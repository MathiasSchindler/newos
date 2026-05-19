# XMLDTDAPPLY

## NAME

xmldtdapply - apply safe DTD-derived defaults to XML

## SYNOPSIS

```
xmldtdapply [--dtd FILE|auto] [--strip-doctype] [FILE ...]
xmldtdapply -h
xmldtdapply --help
```

## DESCRIPTION

The `xmldtdapply` tool reads an XML document, loads DTD declarations, validates the supported DTD constraints, and writes XML with missing defaulted attributes added.

By default `--dtd auto` reads declarations from the document's internal DOCTYPE subset. Use `--dtd FILE` to load declarations from a separate local DTD file. No network resources are fetched.

## CURRENT CAPABILITIES

- parse `<!ELEMENT ...>` and `<!ATTLIST ...>` declarations
- apply default and fixed default attribute values when the attribute is missing
- enforce required attributes before writing output
- enforce fixed attribute values when present
- check declared elements, `EMPTY` elements, ID uniqueness, and IDREF/IDREFS resolution
- optionally remove the DOCTYPE token with `--strip-doctype`

## OPTIONS

- `--dtd FILE|auto` load DTD declarations from FILE, or from the document DOCTYPE when `auto` is used
- `--strip-doctype` omit the source DOCTYPE declaration from output
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- This is not a full DTD processor.
- Content models other than `EMPTY` are preserved for reporting but not fully enforced yet.
- Parameter entities, notations, conditional sections, and entity expansion are not implemented.
- External subsets are loaded only when passed explicitly with `--dtd FILE`; network loading is not implemented.
- Entity references in default values are written as source text and are not expanded first.

## EXAMPLES

```
xmldtdapply document.xml
xmldtdapply --strip-doctype document.xml
xmldtdapply --dtd schema.dtd document.xml > defaulted.xml
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

xmlvalidate, xmldtdinfo, xmlstrip