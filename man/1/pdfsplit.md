# pdfsplit(1)

## Name

pdfsplit - split a PDF into page-range PDFs

## Synopsis

```text
pdfsplit --every N -o PREFIX PDF
pdfsplit --pages A-B [-o OUTPUT] PDF
```

## Description

`pdfsplit` writes new PDFs containing selected pages from an input PDF. With
`--every`, it emits one output per segment using names such as
`PREFIX-001.pdf`, `PREFIX-002.pdf`, and so on. With `--pages`, it writes a
single selected range.

Output PDFs are rebuilt with a fresh catalog, page tree, and cross-reference
table.

## Options

- `--every N` - create one output document for each group of `N` pages
- `--pages A-B` - extract one 1-based inclusive page range; `A` extracts one page
- `-o`, `--output` - output prefix for `--every`, or exact output path for `--pages`
- `-h`, `--help` - show usage

## Limitations

The writer supports normal indirect object PDFs and rejects encrypted files,
xref streams, and compressed object streams. It copies support objects
conservatively, so outputs may retain resources not used by the selected pages.

## Examples

```sh
pdfsplit --every 3 -o chapter input.pdf
pdfsplit --pages 4-7 -o excerpt.pdf input.pdf
```
