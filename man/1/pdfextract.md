# PDFEXTRACT

## NAME

pdfextract - extract diagnostic data from PDF files

## SYNOPSIS

```
pdfextract --stream OBJ[:GEN] [--raw|--decoded] PDF
pdfextract --metadata PDF
```

## DESCRIPTION

`pdfextract` reads one PDF and writes selected diagnostic data to standard
output. `--stream` extracts an indirect object's stream. Streams are decoded by
default when the shared PDF core supports the filter; use `--raw` to copy the
stored bytes exactly.

`--metadata` prints classic document-info fields such as title, author,
creator, producer, and dates when those fields are visible.

## OPTIONS

- `--stream OBJ[:GEN]` - extract the stream from object `OBJ`, optionally with
  generation `GEN`
- `--decoded` - decode supported filters before writing stream data (default)
- `--raw` - write the raw stored stream bytes
- `--metadata` - print document-info metadata fields
- `-h`, `--help` - show usage

## LIMITATIONS

Only unfiltered and single `FlateDecode`/`Fl` streams are decoded. Other
filters, encrypted PDFs, and full XMP interpretation are not supported.

## EXAMPLES

```
pdfextract --stream 5 sample.pdf
pdfextract --stream 5 --raw sample.pdf > stream.bin
pdfextract --metadata sample.pdf
```
