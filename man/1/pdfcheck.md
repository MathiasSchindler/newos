# PDFCHECK

## NAME

pdfcheck - validate basic PDF structure

## SYNOPSIS

```
pdfcheck PDF ...
```

## DESCRIPTION

`pdfcheck` performs shallow structural checks on PDF files. It reports the PDF
header, EOF marker, object and stream counts, root and info references,
xref/object stream notices, encryption notices, and dangling indirect
references found by a simple scanner.

The command exits with status `0` when checks pass and nonzero when the file is
not readable as a PDF or a required structural check fails.

## LIMITATIONS

`pdfcheck` does not repair PDFs, validate cross-reference byte offsets, execute
page content, resolve every incremental-update edge case, or decrypt encrypted
files. Dangling-reference detection is intentionally simple and may report
references that appear in stream bytes.

## EXAMPLES

```
pdfcheck document.pdf
pdfcheck *.pdf
```
