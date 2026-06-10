# pdfsplit(1)

## Name

pdfsplit - split a PDF into page-range PDFs

## Synopsis

```text
pdfsplit --every N -o PREFIX PDF
pdfsplit --pages LIST [-o OUTPUT] PDF
pdfsplit --odd|--even [-o OUTPUT] PDF
```

## Description

`pdfsplit` writes new PDFs containing selected pages from an input PDF. With
`--every`, it emits one output per segment using names such as
`PREFIX-001.pdf`, `PREFIX-002.pdf`, and so on. With `--pages`, `--odd`, or
`--even`, it writes a single selected document.

Output PDFs are rebuilt with a fresh catalog, page tree, and cross-reference
table. Inputs may use ordinary cross-reference tables or modern xref streams
when the needed page and support objects are discoverable.

## Options

- `--every N` - create one output document for each group of `N` pages
- `--pages LIST` - extract 1-based page selectors such as `A`, `A-B`,
  `A-`, `-B`, or comma-separated lists like `1,3-5,9`
- `--odd` - extract odd-numbered pages
- `--even` - extract even-numbered pages
- `-o`, `--output` - output prefix for `--every`, or exact output path for selection modes
- `-h`, `--help` - show usage

## Limitations

The writer supports normal indirect-object PDFs, xref-stream PDFs, and simple
object-stream PDFs whose page/resource dictionaries can be decoded with the
in-tree FlateDecode path. Encrypted files and object streams that cannot be
decoded or faithfully materialized are rejected with a specific error. It copies
support objects conservatively, so outputs may retain resources not used by the
selected pages. Decoded Flate output is capped at 64 MiB, object streams at 8192
objects, and xref streams at 65536 entries.

## Examples

```sh
pdfsplit --every 3 -o chapter input.pdf
pdfsplit --pages 4-7 -o excerpt.pdf input.pdf
pdfsplit --pages 1,3-5,9 -o selected.pdf input.pdf
pdfsplit --even -o even-pages.pdf input.pdf
```
