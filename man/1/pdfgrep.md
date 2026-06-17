# PDFGREP

## NAME

pdfgrep - search simple visible text in PDF content streams

## SYNOPSIS

```
pdfgrep [-i] [-l] [-q] [-n] [-C NUM] PATTERN [PDF ...]
```

## DESCRIPTION

`pdfgrep` scans PDF stream contents, decodes unfiltered and supported
`FlateDecode` streams through the shared PDF core, extracts simple literal and
hex strings used by `Tj`, `TJ`, `'`, and `"` text-showing operators, and searches
for `PATTERN`.

Exit status is grep-like: `0` when a match is found, `1` when no match is found,
and `2` for read or parse errors.

## OPTIONS

- `-i`, `--ignore-case` - ASCII case-insensitive search
- `-l`, `--files-with-matches` - print only matching file names
- `-q`, `--quiet` - suppress output and stop after the first match
- `-n`, `--object-number` - include the matching stream object number
  (object numbers are part of the default output contract)
- `-C NUM`, `--context NUM` - print only `NUM` bytes of simple extracted-text
  context on each side of the first match in a stream, using `...` when clipped
- `-h`, `--help` - show usage

## JSON Output

This command does not provide a JSON output mode.
JSON mode limitation: no JSON output mode is available.

## LIMITATIONS

This is a diagnostic text searcher, not a full PDF text extractor. It does not
apply font encodings, CMaps, layout reconstruction, OCR, or decryption. It
searches simple visible text strings in content streams. Context is byte-based
within the simplified text gathered for one text-showing operation; it is not a
page-layout or line-oriented context mode.

## EXAMPLES

```
pdfgrep "Hello" document.pdf
pdfgrep -i -l invoice *.pdf
pdfgrep -n -C 8 "Total" invoice.pdf
```
