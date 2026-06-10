# PDFGREP

## NAME

pdfgrep - search simple visible text in PDF content streams

## SYNOPSIS

```
pdfgrep [-i] [-l] [-q] PATTERN [PDF ...]
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
- `-h`, `--help` - show usage

## LIMITATIONS

This is a diagnostic text searcher, not a full PDF text extractor. It does not
apply font encodings, CMaps, layout reconstruction, OCR, or decryption. It
searches simple visible text strings in content streams.

## EXAMPLES

```
pdfgrep "Hello" document.pdf
pdfgrep -i -l invoice *.pdf
```
