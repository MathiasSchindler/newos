# XMLDEL

## NAME

xmldel - delete selected XML elements or attributes

## SYNOPSIS

```
xmldel SELECTOR [FILE ...]
xmldel -h
xmldel --help
```

## DESCRIPTION

The `xmldel` tool removes selected element subtrees or selected attributes from XML input.

## CURRENT CAPABILITIES

- remove matching element subtrees
- remove attributes when SELECTOR ends with `/@NAME`
- support final-component attribute and same-name sibling position predicates
- preserve non-matching source text where possible

## OPTIONS

- `SELECTOR` select elements or attributes using the project selector syntax
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Whitespace left around deleted elements is not cleaned up.
- Rewritten tags for attribute deletion use normalized double-quoted attributes.
- Namespace declarations that become unused are not removed.

## EXAMPLES

```
xmldel //debug document.xml
xmldel //item/@temporary document.xml
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

xmlstrip, xmlrename, xmlset