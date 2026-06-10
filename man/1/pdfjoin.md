# pdfjoin(1)

## Name

pdfjoin - combine PDF files into one PDF

## Synopsis

```text
pdfjoin -o OUTPUT [--no-metadata] [--title TEXT] [--author TEXT] [--subject TEXT] [--keywords TEXT] [--creator TEXT] [--producer TEXT] PDF...
```

## Description

`pdfjoin` writes a new PDF containing the pages from two or more input PDFs in
argument order. It copies writable indirect objects, materializes simple
FlateDecode object-stream dictionaries, renumbers references, builds a fresh
catalog and page tree, and writes a new cross-reference table.

By default, the first input document's document-info metadata is used for the
output. Metadata options can override selected fields. Use `--no-metadata` to
write the joined PDF without a document-info dictionary.

## Options

- `-o`, `--output` - write the combined PDF to this path
- `--no-metadata` - omit document-info metadata from the output
- `--title TEXT` - set the output title metadata
- `--author TEXT` - set the output author metadata
- `--subject TEXT` - set the output subject metadata
- `--keywords TEXT` - set the output keywords metadata
- `--creator TEXT` - set the output creator metadata
- `--producer TEXT` - set the output producer metadata
- `-h`, `--help` - show usage

`--no-metadata` cannot be combined with metadata override options.

## Limitations

This writer is intentionally conservative. It supports normal indirect-object
PDFs, PDFs with xref streams when the needed objects are discoverable, and
simple object-stream PDFs whose page/resource dictionaries can be decoded with
the in-tree FlateDecode path. Encrypted files and object streams that cannot be
decoded or faithfully materialized are rejected with a specific error. Page
dictionaries with direct resource dictionaries are handled best; very complex
inherited page-tree resources may need a future, deeper page-tree model.
Decoded Flate output is capped at 64 MiB, object streams at 8192 objects, and
xref streams at 65536 entries.

Bookmarks and outlines are not preserved yet; the current writer focuses on
page/object joining and document-info metadata.

## Examples

```sh
pdfjoin -o combined.pdf part1.pdf part2.pdf part3.pdf
```
