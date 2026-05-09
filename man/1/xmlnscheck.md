# XMLNSCHECK

## NAME

xmlnscheck - check basic XML namespace prefix usage

## SYNOPSIS

```
xmlnscheck [FILE ...]
xmlnscheck --unused [FILE ...]
xmlnscheck -h
xmlnscheck --help
```

## DESCRIPTION

The `xmlnscheck` tool checks basic namespace prefix binding rules while still relying on the shared well-formedness parser.

## CURRENT CAPABILITIES

- detect unbound element and attribute prefixes
- detect duplicate namespace declarations on one element
- check basic reserved prefix misuse for `xml` and `xmlns`
- optionally report unused prefixed namespace declarations with `--unused`
- read one or more files or standard input

## OPTIONS

- `--unused` report prefixed namespace declarations that are not used before leaving scope
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- This is a namespace linter, not a full namespace-aware validator.
- Unused default namespace declarations are not reported.
- `--unused` checks prefix use textually; it does not resolve expanded names for selectors or other tools.
- Selectors elsewhere in the toolkit still match textual names rather than resolved namespace URIs.

## EXAMPLES

```
xmlnscheck document.xml
xmlnscheck --unused document.xml
xmlnscheck *.xml
```

## SEE ALSO

xmlcheck, xmlvalidate, xmlsafe