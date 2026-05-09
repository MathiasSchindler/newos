# XMLDTDINFO

## NAME

xmldtdinfo - print a summary of supported DTD declarations

## SYNOPSIS

```
xmldtdinfo [--dtd FILE] [FILE ...]
xmldtdinfo -h
xmldtdinfo --help
```

## DESCRIPTION

The `xmldtdinfo` tool prints the supported DTD declarations known for an XML document or standalone DTD file. Without `--dtd`, each input is treated as XML and declarations are read from its internal DOCTYPE subset. With `--dtd FILE`, the named DTD file is inspected directly.

## CURRENT CAPABILITIES

- report the DOCTYPE root name when available
- list parsed element declarations and their content text
- list parsed attribute declarations, types, and defaults
- read internal DOCTYPE subsets or explicit local DTD files

## OPTIONS

- `--dtd FILE` inspect declarations from FILE instead of reading declarations from XML input
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Only `ELEMENT` and `ATTLIST` declarations are reported.
- Parameter entities, notations, conditional sections, and general entity declarations are not modeled yet.
- External subsets referenced by DOCTYPE system identifiers are not loaded automatically.

## EXAMPLES

```
xmldtdinfo document.xml
xmldtdinfo --dtd schema.dtd
```

## SEE ALSO

xmlvalidate, xmldtdapply, xmltokens