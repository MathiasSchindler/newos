# XMLHEAD

## NAME

xmlhead - print the first matching XML element subtrees

## SYNOPSIS

```
xmlhead [-n COUNT] [--wrap NAME] SELECTOR [FILE ...]
xmlhead -h
xmlhead --help
```

## DESCRIPTION

The `xmlhead` tool prints the first COUNT selected element subtrees. The default count is 10. By default the output is a newline-delimited XML fragment stream; use `--wrap NAME` to synthesize one wrapper element around all selected subtrees.

## CURRENT CAPABILITIES

- select element subtrees with the project selector syntax
- support final-component attribute and same-name sibling position predicates
- stop after the requested number of matches
- optionally wrap all matches in a single element with `--wrap NAME`
- read one or more files or standard input

## OPTIONS

- `-n COUNT` print at most COUNT selected subtrees
- `--count COUNT` print at most COUNT selected subtrees
- `--wrap NAME` emit `<NAME>` before matches and `</NAME>` after matches
- `SELECTOR` select element subtrees using the project selector syntax
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Only element subtrees are printed.
- Without `--wrap`, multiple matches are XML fragments, not one well-formed document.
- Extracted subtrees are not reformatted.
- Ancestor namespace declarations are not added when they live outside the selected subtree.

## EXAMPLES

```
xmlhead -n 5 //entry feed.xml
xmlhead -n 5 --wrap results //entry feed.xml
xmlhead //item document.xml
```

## SEE ALSO

xmltail, xmlcut, xmlsplit