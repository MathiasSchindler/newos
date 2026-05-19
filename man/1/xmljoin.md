# XMLJOIN

## NAME

xmljoin - join selected XML elements from two documents by key

## SYNOPSIS

```
xmljoin SELECTOR KEY LEFT.xml RIGHT.xml
xmljoin -h
xmljoin --help
```

## DESCRIPTION

The `xmljoin` tool pairs selected elements from two XML documents by matching textual key values. Output is a synthetic `<joined>` document containing one `<join>` record for each matched key from the left input.

## CURRENT CAPABILITIES

- select element subtrees with the project selector syntax
- support final-component attribute and same-name sibling position predicates
- use an attribute, direct text, or direct child text value as the join key
- emit inner joins for keys found in both inputs
- index right-hand matches by textual key before output
- grow selected-item storage as matches are encountered

## OPTIONS

- `SELECTOR` select element subtrees in both inputs using the project selector syntax
- `KEY` use `@ATTR`, `.`, or a direct child element name as the join key
- `LEFT.xml` read the left input XML document
- `RIGHT.xml` read the right input XML document
- `-h`, `--help` print a short usage line

## LIMITATIONS

- This is a simple subtree pairing tool, not a relational join engine.
- Only inner joins are supported.
- Left, right, full outer, and anti-join modes are not implemented yet.
- Output shape is fixed to `<joined><join>...</join></joined>`.
- Direct text and child keys use the first non-blank direct text node only.
- Child keys do not search through nested descendants.
- Matching elements are kept in process memory.
- Attributes and child nodes are not merged into the original document shape.

## EXAMPLES

```
xmljoin //item @id old.xml new.xml
xmljoin //item code left.xml right.xml
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

xmlsort, xmluniq, xmlcut