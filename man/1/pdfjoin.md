# pdfjoin(1)

## Name

pdfjoin - combine PDF files into one PDF

## Synopsis

```text
pdfjoin -o OUTPUT PDF...
```

## Description

`pdfjoin` writes a new PDF containing the pages from two or more input PDFs in
argument order. It copies ordinary indirect objects, renumbers references, builds
a fresh catalog and page tree, and writes a new cross-reference table.

The first input document's document-info metadata is used for the output.

## Options

- `-o`, `--output` - write the combined PDF to this path
- `-h`, `--help` - show usage

## Limitations

This first writer is intentionally conservative. It supports normal indirect
object PDFs and rejects encrypted files, xref streams, and compressed object
streams. Page dictionaries with direct resource dictionaries are handled best;
very complex inherited page-tree resources may need a future, deeper page-tree
model.

## Examples

```sh
pdfjoin -o combined.pdf part1.pdf part2.pdf part3.pdf
```
