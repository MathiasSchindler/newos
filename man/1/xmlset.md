# XMLSET

## NAME

xmlset - set selected XML element text or attribute values

## SYNOPSIS

```
xmlset [--force] SELECTOR VALUE [FILE ...]
xmlset -h
xmlset --help
```

## DESCRIPTION

The `xmlset` tool replaces selected element content with text or sets selected attribute values. Missing selected attributes are added to matching elements.

Replacement values are always treated as text and escaped on output. They are never parsed as XML markup, which prevents replacement text from injecting new tags.

## CURRENT CAPABILITIES

- replace text-only selected element content with escaped text
- require `--force` before replacing selected elements that contain child elements
- set selected attribute values
- add a missing selected attribute to matching elements
- support final-component attribute and same-name sibling position predicates
- validate attribute names before adding missing attributes
- escape text and attribute values when writing replacements

## OPTIONS

- `--force` allow selected element replacement even when the element contains child elements
- `SELECTOR` select elements or attributes using the project selector syntax
- `VALUE` replacement text or attribute value
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Element replacement has no append or prepend mode yet.
- Rewritten tags use normalized double-quoted attributes.
- There is no mode for parsing replacement XML fragments as markup.

## EXAMPLES

```
xmlset //title "New title" document.xml
xmlset --force //chapter "TODO" document.xml
xmlset //item/@status active document.xml
```

## SEE ALSO

xmlrename, xmldel, xmlcut