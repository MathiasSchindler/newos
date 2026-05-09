# XML2CSV

## NAME

xml2csv - extract simple row-oriented XML into CSV

## SYNOPSIS

```
xml2csv [--header] [--sep CHAR] --row SELECTOR --col NAME [--col NAME ...] [FILE ...]
xml2csv -h
xml2csv --help
```

## DESCRIPTION

The `xml2csv` tool extracts simple row-oriented XML into comma-separated values. Each matching row element becomes one CSV record.

## CURRENT CAPABILITIES

- extract attributes from the row element with `@NAME` columns
- extract direct row text with the `.` column
- extract direct child element text with NAME columns
- support final-component attribute and same-name sibling position predicates in the row selector
- optionally emit a header row with `--header`
- use a custom one-character separator with `--sep CHAR`
- quote CSV cells when values contain commas, quotes, or newlines
- handle repeated row matches

## OPTIONS

- `--header` emit a header row before data rows
- `--sep CHAR` use CHAR instead of comma as the field separator; common escapes such as `\t` are accepted
- `--row SELECTOR` select row elements using the project selector syntax
- `--col NAME` add one output column; repeat for multiple columns
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Each column selects one simple row attribute, direct row text value, or direct child text value.
- Nested grandchildren and multiple same-name child elements are not flattened or combined.
- Descendant text is not joined into a single column value.
- There is no multi-value join mode for repeated same-name child elements yet.

## EXAMPLES

```
xml2csv --row //item --col @id --col . document.xml
xml2csv --header --sep '\t' --row //item --col @id --col title document.xml
xml2csv --row //book --col title --col author catalog.xml
```

## SEE ALSO

xml2json, xml2lines, xmlget