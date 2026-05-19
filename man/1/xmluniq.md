# XMLUNIQ

## NAME

xmluniq - remove duplicate selected XML elements by key

## SYNOPSIS

```
xmluniq SELECTOR KEY [FILE ...]
xmluniq -h
xmluniq --help
```

## DESCRIPTION

The `xmluniq` tool keeps the first selected element for each textual key value and removes later duplicate selected subtrees.

## CURRENT CAPABILITIES

- select element subtrees with the project selector syntax
- support final-component attribute and same-name sibling position predicates
- use an attribute, direct text, or direct child text value as the duplicate key
- keep the first selected element for each key
- preserve other XML source text as tokenized
- grow key storage as distinct keys are encountered

## OPTIONS

- `SELECTOR` select element subtrees using the project selector syntax
- `KEY` use `@ATTR`, `.`, or a direct child element name as the duplicate key
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Direct text and child keys use the first non-blank direct text node only.
- Child keys do not search through nested descendants.
- Duplicate-reporting modes like `--duplicates`, `--unique-only`, counts, and `--keep=last` are not implemented yet.
- Composite keys such as `@namespace,@title` are not implemented yet.
- Key storage is kept in process memory.
- Whitespace around removed subtrees is not cleaned up.

## EXAMPLES

```
xmluniq //item @id catalog.xml
xmluniq //item code catalog.xml
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

xmlsort, xmljoin, xmldel