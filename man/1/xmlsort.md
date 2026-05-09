# XMLSORT

## NAME

xmlsort - extract selected XML elements sorted by key

## SYNOPSIS

```
xmlsort [-n] [-r] SELECTOR KEY [FILE ...]
xmlsort -h
xmlsort --help
```

## DESCRIPTION

The `xmlsort` tool collects selected element subtrees, sorts them by a textual key, and writes one sorted subtree per line.

## CURRENT CAPABILITIES

- select element subtrees with the project selector syntax
- support final-component attribute and same-name sibling position predicates
- sort selected subtrees by attribute, direct text, or direct child text key
- sort numeric integer keys with `-n` or `--numeric`
- reverse sort order with `-r` or `--reverse`
- preserve input order for equal keys
- grow selected-item storage as matches are encountered

## OPTIONS

- `-n`, `--numeric` compare integer key values numerically; non-numeric keys sort after numeric keys
- `-r`, `--reverse` reverse the key comparison order
- `SELECTOR` select element subtrees using the project selector syntax
- `KEY` use `@ATTR`, `.`, or a direct child element name as the sort key
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- The original full document is not rewritten in place.
- Direct text and child keys use the first non-blank direct text node only.
- Child keys do not search through nested descendants.
- Source documents and selected-item records remain buffered until sorted output is written, so peak memory can approach multiple copies of selected input.
- No external merge sort, temporary-file spill, or `--keys-only` byte-offset mode is implemented yet.
- Sorting is bytewise textual by default, not locale-aware.

## EXAMPLES

```
xmlsort //item @id catalog.xml
xmlsort -n //item @rank catalog.xml
xmlsort -r //item name catalog.xml
xmlsort //item name catalog.xml
xmlsort //item . values.xml
```

## SEE ALSO

xmluniq, xmljoin, sort