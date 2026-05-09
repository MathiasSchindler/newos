# XMLMIN

## NAME

xmlmin - remove insignificant blank XML text nodes

## SYNOPSIS

```
xmlmin [FILE ...]
xmlmin -h
xmlmin --help
```

## DESCRIPTION

The `xmlmin` tool writes compact XML by dropping text nodes that contain only XML whitespace. It reads one or more files, or standard input when no file is named or `-` is used.

## CURRENT CAPABILITIES

- drop blank text nodes used for indentation
- preserve tags, attributes, non-blank text, comments, CDATA, processing instructions, and DOCTYPE tokens
- read one or more files or standard input

## OPTIONS

- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Comments are not removed; use `xmlstrip --comments` for that.
- Entity references, attributes, and namespace spelling are not normalized.
- Whitespace inside non-blank text nodes is preserved.

## EXAMPLES

```
xmlmin document.xml > document.min.xml
xmlmin document.xml | xmlcheck
```

## SEE ALSO

xmlfmt, xmlstrip, xmlcanon