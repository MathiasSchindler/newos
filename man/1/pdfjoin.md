# pdfjoin(1)

## Name

pdfjoin - combine PDF files into one PDF

## Synopsis

```text
pdfjoin -o OUTPUT [--no-metadata] [--title TEXT] [--author TEXT] [--subject TEXT] [--keywords TEXT] [--creator TEXT] [--producer TEXT] PDF...
```

## Description

`pdfjoin` writes a new PDF containing the pages from two or more input PDFs in
argument order. It copies ordinary indirect objects, renumbers references, builds
a fresh catalog and page tree, and writes a new cross-reference table.

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

This first writer is intentionally conservative. It supports normal indirect
object PDFs and rejects encrypted files, xref streams, and compressed object
streams. Page dictionaries with direct resource dictionaries are handled best;
very complex inherited page-tree resources may need a future, deeper page-tree
model.

Bookmarks and outlines are not preserved yet; the current writer focuses on
page/object joining and document-info metadata.

## Examples

```sh
pdfjoin -o combined.pdf part1.pdf part2.pdf part3.pdf
```
