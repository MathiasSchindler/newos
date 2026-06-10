# PDFCHECK

## NAME

pdfcheck - validate basic PDF structure

## SYNOPSIS

```
pdfcheck [--json] PDF ...
```

## DESCRIPTION

`pdfcheck` performs shallow structural checks on PDF files. It reports the PDF
header, EOF marker, object and stream counts, root and info references,
xref/object stream notices, encryption notices, and dangling indirect
references found by a simple scanner.

The command exits with status `0` when checks pass and nonzero when the file is
not readable as a PDF or a required structural check fails.

Use `--json` to emit a script-friendly JSON array. Each entry includes the file
name, overall `ok` status, header/EOF status, counters, root/info references,
notices, and errors. Strings are JSON-escaped.

## OPTIONS

- `--json` - write machine-readable JSON instead of human output
- `-h`, `--help` - show usage

## LIMITATIONS

`pdfcheck` does not repair PDFs, validate cross-reference byte offsets, execute
page content, resolve every incremental-update edge case, or decrypt encrypted
files. Dangling-reference detection skips stream bodies so content bytes are not
reported as object references. Shared parser defensive caps apply: decoded Flate
output is capped at 64 MiB, object streams at 8192 objects, and xref streams at
65536 entries.

## EXAMPLES

```
pdfcheck document.pdf
pdfcheck --json document.pdf
pdfcheck *.pdf
```
