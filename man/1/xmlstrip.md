# XMLSTRIP

## NAME

xmlstrip - remove selected XML metadata tokens

## SYNOPSIS

```
xmlstrip [--comments] [--pi] [--doctype] [--all] [FILE ...]
xmlstrip -h
xmlstrip --help
```

## DESCRIPTION

The `xmlstrip` tool removes comments, processing instructions, and DOCTYPE declarations from XML input. At least one stripping option is required.

## CURRENT CAPABILITIES

- remove comments with `--comments`
- remove processing instructions with `--pi`
- remove DOCTYPE declarations with `--doctype`
- remove all supported metadata token types with `--all`
- preserve all other XML source text as tokenized

## OPTIONS

- `--comments` remove XML comments
- `--pi` remove processing instructions
- `--doctype` remove DOCTYPE declarations
- `--all` remove comments, processing instructions, and DOCTYPE declarations
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Whitespace around stripped tokens is not removed.
- There is no `--tidy` whitespace cleanup mode yet.
- Elements and attributes are not removed.
- Remaining XML is not reformatted or rewritten.

## EXAMPLES

```
xmlstrip --comments document.xml
xmlstrip --all document.xml
```

## SEE ALSO

xmlmin, xmldel, xmlsafe