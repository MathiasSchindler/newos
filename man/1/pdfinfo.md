# PDFINFO

## NAME

pdfinfo - show structural PDF metadata

## SYNOPSIS

```
pdfinfo [-p|--plain] [-d|--details] [--pages] [--objects] [--fonts] [--json] [file ...]
```

## DESCRIPTION

`pdfinfo` scans PDF files and reports document structure that can be discovered
without executing PDF content. It recognizes the PDF header, indirect objects,
streams, xref markers and xref streams, trailers, pages, page boxes, fonts,
image and form XObjects, filters, encodings, annotations, metadata objects,
classic document-info fields, and compressed object streams. With `--details`,
it also counts a small set of visible content-stream operators.

When no file is provided, `pdfinfo` reads from standard input.

The PDF parser lives in the shared library code so future PDF tools can reuse
the same object, page, font, filter, and content-operator analysis.

## OPTIONS

- `-p`, `--plain` - print one compact key/value line per input
- `-d`, `--details` - include extra counters, content-operator counts, and list pages and fonts
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
title: Example Document
author: A. Writer
creator: WriterApp
producer: PDF Library
creation_date: 2026-06-09 12:00:00 UTC (raw D:20260609120000Z)
modification_date: 2026-06-09 12:30:00 UTC+01:00 (raw D:20260609123000+01'00')
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
prints one JSON object per input with the same summary counters, name
histograms, and page details.

Document-info metadata is printed when present. Supported fields are `title`,
`author`, `subject`, `keywords`, `creator`, `producer`, `creation_date`, and
`modification_date`. Human output formats PDF date strings as
`YYYY-MM-DD HH:MM:SS UTC` or `YYYY-MM-DD HH:MM:SS UTC+HH:MM` and keeps the raw
PDF value in parentheses. In PDF date strings, `Z` means UTC, and suffixes such
as `+01'00'` or `-05'30'` are timezone offsets. JSON output preserves the raw
`creation_date` and `modification_date` values and also includes
`creation_date_formatted` and `modification_date_formatted`.

Page dimensions are reported in PDF points. Known page boxes are matched
approximately against common formats such as Letter, Legal, A3, A4, and A5.

## JSON Output

With `--json`, `pdfinfo` emits one compact JSON object per input file. Existing
summary fields are preserved, and `page_details` contains one object for each
discovered page. Each page detail includes `page_number`, `object_number`, and
`generation`; it also includes `media_box` and `crop_box` four-number point
arrays, `rotation`, and `page_format` when those values are visible in the PDF.

## LIMITATIONS

- The analyzer is a shallow structural scanner. It can decode unfiltered and
  single `FlateDecode`/`Fl` streams for xref streams, object streams, and simple
  content scanning, but it does not repair malformed files, follow every
  incremental update revision, or execute page content.
- Defensive caps reject or ignore expensive modern structures: decoded Flate
  output is capped at 64 MiB, object streams at 8192 objects, and xref streams
  at 65536 entries.
- Filter and encoding names are counted when they are visible in object
  dictionaries. Unsupported filtered stream contents are not interpreted.
- Classic document-info metadata is read from visible object dictionaries using
  literal strings, name values, and hex strings with UTF-16 byte-order marks.
  XMP packet fields inside compressed metadata streams are not decoded.
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