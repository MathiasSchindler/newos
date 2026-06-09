# pdfinfoedit(1)

## Name

pdfinfoedit - edit PDF document-info metadata

## Synopsis

```text
pdfinfoedit --set FIELD=VALUE [--remove FIELD] -o OUTPUT PDF
```

## Description

`pdfinfoedit` updates classic PDF document-info metadata. It preserves the input
bytes and appends an incremental update containing a new info dictionary and
cross-reference section.

Supported fields are `title`, `author`, `subject`, `keywords`, `creator`,
`producer`, `creation_date`, and `modification_date`. Date values are written as
provided; PDF dates usually use forms such as `D:20260609120000Z` or
`D:20260609123000+01'00'`.

## Options

- `--set FIELD=VALUE` - set a metadata field; may be repeated
- `--remove FIELD` - remove a metadata field from the newly written info dictionary
- `-o`, `--output` - write the updated PDF to this path
- `-h`, `--help` - show usage

## Limitations

The tool edits classic document-info metadata, not XMP metadata packets. It is
conservative and rejects encrypted files, xref streams, and compressed object
streams.

## Examples

```sh
pdfinfoedit --set title='New Title' --set author='A. Writer' -o edited.pdf input.pdf
pdfinfoedit --remove keywords -o edited.pdf input.pdf
```
