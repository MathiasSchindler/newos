# XMLQUERY

## NAME

xmlquery - extract XML subtrees with shared selector predicates

## SYNOPSIS

```
xmlquery [--wrap NAME] SELECTOR [FILE ...]
xmlquery -h
xmlquery --help
```

## DESCRIPTION

The `xmlquery` tool extracts selected XML subtrees. It uses the shared project selector syntax, including final-element attribute and sibling-position predicates. By default the output is a newline-delimited XML fragment stream; use `--wrap NAME` to synthesize one wrapper element around all selected subtrees.

`xmlquery` also supports final-element text equality predicates as a query-only extension. Use `[.="text"]` or `[text()="text"]` to require the selected element's concatenated text and CDATA descendants to equal the supplied value.

## CURRENT CAPABILITIES

- select element subtrees with the project selector syntax
- filter the final selected element with `[@name]` and `[@name=value]`
- accept quoted or unquoted predicate values
- combine repeated final-element predicates such as `[@status][@group=a]`
- filter by same-name sibling position with `[N]`
- filter by final-element text with `[.="text"]` or `[text()="text"]`
- write matching subtrees, one per line
- optionally wrap all matches in a single element with `--wrap NAME`

## OPTIONS

- `--wrap NAME` emit `<NAME>` before matches and `</NAME>` after matches
- `SELECTOR` select element subtrees using the project selector syntax and optional final-element predicates
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- This is not XPath.
- Without `--wrap`, multiple matches are XML fragments, not one well-formed document.
- Predicates are supported only on the final selector component.
- Predicate matching compares source attribute values textually; entities are not decoded first.
- Text predicates compare concatenated source text and CDATA descendants textually; entity references are not decoded first.
- `[N]` is same-name sibling position under the parent, not full XPath node-set position after earlier predicates.
- `last()`, boolean expressions, and general XPath functions are not implemented.

## EXAMPLES

```
xmlquery //item file.xml
xmlquery --wrap results //item file.xml
xmlquery '//item[@id=123]' file.xml
xmlquery '//item[@status="active"]' file.xml
xmlquery '//item[@status][@group=a]' file.xml
xmlquery '//item[2]' file.xml
xmlquery '//item[.="ready"]' file.xml
xmlquery '//item[text()="ready"]' file.xml
```

## SEE ALSO

xmlcut, xmlget, xmlgrep