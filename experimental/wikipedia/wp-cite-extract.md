# WP-CITE-EXTRACT

## NAME

wp-cite-extract - extract citation identifiers from Wikipedia dump XML

## SYNOPSIS

```
wp-cite-extract [-q] [-d DIR] [-i FILE] [-o FILE]
```

## DESCRIPTION

`wp-cite-extract` scans MediaWiki XML dump files and writes a TSV citation cache.
It is intended as the expensive first pass before later matching against data
sets such as Retraction Watch.

When no input file is specified, the tool searches the data directory for
`*wiki-YYYY-MM-DD-*.xml` and `*.xml.bz2` files, chooses the newest snapshot date,
and processes every file for that wiki/date set. The default data directory is
`experimental/wikipedia/data` when run from the repository root and `data` when
run from `experimental/wikipedia`.

The extractor currently scans namespace 0 article pages. It emits heuristic
records for raw DOI, PMID, ISBN, and ISSN values, German ISBN signals including
`978-3` and `9783`, `<ref>` blocks containing citation signals, and citation
templates such as `{{Literatur}}`, `{{Internetquelle}}`, and `{{Cite...}}`.

The TSV columns are:

```text
wiki	snapshot	source	page_id	page_title	kind	value	raw
```

`kind` is one of `doi`, `pmid`, `isbn`, `issn`, `ref`, or `template`. Identifier
rows place the normalized-looking extracted token in `value`; contextual rows
such as `ref` and `template` leave `value` empty and put a short TSV-escaped
snippet in `raw`.

## OPTIONS

- `-q` / `--quiet` - suppress progress and status messages
- `-d DIR` / `--data-dir DIR` - search DIR for dump files and write the default
  output there
- `-i FILE` / `--input FILE` - process one XML or XML.BZ2 dump file instead of
  discovering the newest snapshot set
- `-o FILE` / `--output FILE` - write the TSV cache to FILE instead of the
  default `<wiki>-<snapshot>-citations.tsv` path

## EXAMPLES

```
wp-cite-extract
wp-cite-extract -d experimental/wikipedia/data
wp-cite-extract -i experimental/wikipedia/data/dewiki-2026-06-01-p1p3456636.xml.bz2
wp-cite-extract -q -o experimental/wikipedia/data/dewiki-citations.tsv
```

## LIMITATIONS

- Citation extraction is intentionally heuristic; it favors recall for a later
  matching pipeline over perfect citation parsing.
- `.xml.bz2` input uses the in-tree shared bzip2 decoder; plain `.xml` input is
  parsed directly.
- Retraction Watch matching is not part of this tool yet; this command writes
  the reusable citation cache that the matcher can consume.

## SEE ALSO

wp-download, bzip2