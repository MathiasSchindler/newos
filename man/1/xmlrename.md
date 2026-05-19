# XMLRENAME

## NAME

xmlrename - rename selected XML elements or attributes

## SYNOPSIS

```
xmlrename SELECTOR NEW-NAME [FILE ...]
xmlrename -h
xmlrename --help
```

## DESCRIPTION

The `xmlrename` tool renames selected elements or attributes while preserving non-matching source text where possible.

## CURRENT CAPABILITIES

- rename all elements matching a selector, including matching end tags
- rename attributes when SELECTOR ends with `/@NAME`
- support final-component attribute and same-name sibling position predicates
- accept replacement attribute names with or without a leading `@`
- validate replacement element and attribute names before writing output

## OPTIONS

- `SELECTOR` select elements or attributes using the project selector syntax
- `NEW-NAME` replacement element or attribute name
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Rewritten tags use normalized double-quoted attributes.
- Attribute selectors match attributes on matching elements only.
- Namespace declarations are not rewritten to preserve namespace semantics.

## EXAMPLES

```
xmlrename //old new document.xml
xmlrename //item/@old newName document.xml
xmlrename //item/@old @newName document.xml
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

xmlset, xmldel, xmlcanon