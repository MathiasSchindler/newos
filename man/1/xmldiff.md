# XMLDIFF

## NAME

xmldiff - compare XML files as parser token streams

## SYNOPSIS

```
xmldiff LEFT.xml RIGHT.xml
xmldiff -h
xmldiff --help
```

## DESCRIPTION

The `xmldiff` tool compares two XML files as parser token streams. It reports whether they are equivalent under the tool's comparison rules and identifies the first differing token.

## CURRENT CAPABILITIES

- ignore indentation-only text nodes
- compare element names, text, CDATA, comments, processing instructions, DOCTYPE text, and attributes
- treat attributes as equal even when their order differs
- print `equal` for equivalent token streams
- print `different at token N` for the first detected difference

## OPTIONS

- `LEFT.xml` first XML file to compare
- `RIGHT.xml` second XML file to compare
- `-h`, `--help` print a short usage line

## LIMITATIONS

- No unified diff or patch output is produced.
- No XML patch instruction format is emitted yet, so there is no `xmlpatch` companion tool.
- No path-style diff output, brief mode, or strict attribute-order mode is implemented yet.
- Entity references are not decoded before comparison.
- Namespace URI comparison is not performed.

## EXAMPLES

```
xmldiff old.xml new.xml
xmldiff formatted.xml minified.xml
```

## SEE ALSO

xmlcanon, xmlfmt, xmlcheck