# XMLCUT

## NAME

xmlcut - extract XML element subtrees by selector

## SYNOPSIS

```
xmlcut [--wrap NAME] SELECTOR [FILE ...]
xmlcut -h
xmlcut --help
```

## DESCRIPTION

The `xmlcut` tool writes each selected element subtree as XML. By default the output is a newline-delimited XML fragment stream; use `--wrap NAME` to synthesize one wrapper element around all selected subtrees. It reads one or more files, or standard input when no file is named or `-` is used.

## CURRENT CAPABILITIES

- select elements with the project path selector syntax
- support final-component attribute and same-name sibling position predicates
- preserve original source spelling inside each selected subtree
- write each matching subtree to standard output
- optionally wrap all matches in a single element with `--wrap NAME`

## OPTIONS

- `--wrap NAME` emit `<NAME>` before matches and `</NAME>` after matches
- `SELECTOR` select element subtrees using the project selector syntax
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Attributes and arbitrary text ranges are not selected directly.
- Without `--wrap`, multiple matches are XML fragments, not one well-formed document.
- Extracted subtrees are not reformatted.
- Ancestor namespace declarations are not added when they live outside the selected subtree.

## EXAMPLES

```
xmlcut //item document.xml
xmlcut --wrap results //item document.xml
xmlcut /feed/entry feed.xml
```

## SEE ALSO

xmlget, xmlquery, xmlsplit