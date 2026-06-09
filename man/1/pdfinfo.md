# PDFINFO

## NAME

pdfinfo - show structural PDF metadata

## SYNOPSIS

```
pdfinfo [-p|--plain] [-d|--details] [--pages] [--objects] [--fonts] [--json] [file ...]
```

## DESCRIPTION

`pdfinfo` scans PDF files and reports document structure that can be discovered
without executing PDF content or inflating compressed streams. It recognizes the
PDF header, indirect objects, streams, xref markers, trailers, pages, page
boxes, fonts, image and form XObjects, filters, encodings, annotations,
metadata objects, and a small set of visible content-stream operators.

When no file is provided, `pdfinfo` reads from standard input.

The PDF parser lives in the shared library code so future PDF tools can reuse
the same object, page, font, filter, and content-operator analysis.

## OPTIONS

- `-p`, `--plain` - print one compact key/value line per input
- `-d`, `--details` - include extra counters and list pages and fonts
- `--pages` - list discovered page objects and page sizes
- `--objects` - list discovered indirect objects
- `--fonts` - list discovered font objects
- `--json` - print one JSON object per input
- `-h`, `--help` - show usage

## OUTPUT

The default output is a labeled summary:

```
file: document.pdf
pdf_version: 1.7
bytes: 10240
objects: 43
streams: 18
pages: 2
fonts: 3
images: 4
encrypted: no
has_eof: yes
filters: FlateDecode(12)
encodings: WinAnsiEncoding(1)
```

`--plain` prints a single line with key/value pairs suitable for scripts.
`--details` adds structural counters, page entries, and font entries. `--json`
prints a JSON object with the same summary counters and name histograms.

Page dimensions are reported in PDF points. Known page boxes are matched
approximately against common formats such as Letter, Legal, A3, A4, and A5.

## LIMITATIONS

- The analyzer is a shallow structural scanner. It does not decompress streams,
  repair malformed files, evaluate object streams, follow incremental update
  revisions, or execute page content.
- Filter and encoding names are counted when they are visible in object
  dictionaries. Filtered stream contents are not interpreted.
- Font embedding is detected from visible `FontFile`, `FontFile2`, or
  `FontFile3` entries in the same object dictionary.
- The encrypted flag is reported when an `/Encrypt` key is visible to the
  scanner; encrypted file contents are not decrypted.

## EXAMPLES

```
pdfinfo document.pdf
pdfinfo --details document.pdf
pdfinfo --pages --fonts document.pdf
pdfinfo --plain *.pdf
pdfinfo --json document.pdf
cat document.pdf | pdfinfo
```

## SEE ALSO

file, strings, hexdump, imginfo