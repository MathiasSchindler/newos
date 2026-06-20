# WP-RETRACTION-MATCH

## NAME

wp-retraction-match - match Wikipedia citation TSV rows against Retraction Watch

## SYNOPSIS

```
wp-retraction-match [-q] [-d DIR] [-c CITATIONS.tsv] [-r RETRACTION_WATCH.csv] [-o MATCHES.tsv] [-w WEAK.tsv] [--no-weak] [--article-dedup FILE]
```

## DESCRIPTION

`wp-retraction-match` is the second-stage processor for the experimental
Wikipedia citation pipeline. It reads a TSV cache produced by `wp-cite-extract`
and reports Wikipedia article citation rows whose DOI or PubMed identifier
matches a retracted original paper in the Retraction Watch CSV export.

The hard matcher indexes Retraction Watch `OriginalPaperDOI` and
`OriginalPaperPubMedID` values in memory, then streams the citation TSV. It does
not match `RetractionDOI` or `RetractionPubMedID` in the hard output because
those identify the retraction notice rather than the original paper being cited.
Weak title/context matches, when enabled, are written to a separate weak output
so DOI/PMID evidence stays separate from weaker signals.

When no citation TSV is specified, the tool searches the data directory for the
newest `*wiki-YYYY-MM-DD-citations.tsv`. The default data directory is
`experimental/wikipedia/data` when run from the repository root and `data` when
run from `experimental/wikipedia`. The default Retraction Watch input is
`retraction_watch.csv` in that data directory, and the default output is
`<wiki>-<snapshot>-retraction-matches.tsv`.

The output columns are:

```text
match_kind	match_value	match_field	wiki	snapshot	source	page_id	page_title	citation_kind	citation_value	rw_record_id	rw_original_doi	rw_original_pmid	rw_retraction_doi	rw_retraction_pmid	rw_retraction_date	rw_original_date	rw_retraction_nature	rw_title	rw_journal	rw_publisher	rw_reason	raw
```

The hard output intentionally remains one row per matched evidence row. A single
article can therefore have several rows for the same Retraction Watch record
because `wp-cite-extract` emits DOI, PMID, template, and reference context rows,
or because the same article cites the same retracted work more than once.
Non-quiet runs report article-level duplicate counters on standard error,
separately for hard DOI and hard PMID matches. The article-level hard duplicate
key is the wiki, snapshot, page id, Retraction Watch record, hard match kind, and
normalized match value.

`--article-dedup FILE` writes an auxiliary hard-match TSV with one row per
article-level hard duplicate key. It prefixes the hard output columns with:

```text
evidence_row_count
```

The auxiliary file uses the first raw evidence row as the representative and
keeps the full raw hard-match output unchanged.

## OPTIONS

- `-q` / `--quiet` - suppress progress and status messages
- `-d DIR` / `--data-dir DIR` - search DIR for default inputs and write the
  default output there
- `-c FILE` / `--citations FILE` - process one citation TSV instead of
  discovering the newest cache
- `-r FILE` / `--retractions FILE` - read Retraction Watch data from FILE
  instead of `retraction_watch.csv`
- `-o FILE` / `--output FILE` - write matches to FILE instead of the default
  `<wiki>-<snapshot>-retraction-matches.tsv`; use `-` for standard output
- `-w FILE` / `--weak-output FILE` - write weak-signal matches to FILE instead
  of the default `<wiki>-<snapshot>-weak-retraction-matches.tsv`
- `--no-weak` - disable weak-signal output
- `--article-dedup FILE` - write article-level deduplicated hard matches to
  FILE; use this as a summary alongside the raw hard output

## EXAMPLES

```
wp-retraction-match
wp-retraction-match -d experimental/wikipedia/data
wp-retraction-match -c experimental/wikipedia/data/dewiki-2026-06-01-citations.tsv
wp-retraction-match -q -o experimental/wikipedia/data/dewiki-retraction-matches.tsv
wp-retraction-match --article-dedup experimental/wikipedia/data/dewiki-retraction-matches.article.tsv
```

## LIMITATIONS

- Hard matching is identifier-based and currently uses only DOI and PubMed ID
  rows emitted by `wp-cite-extract`.
- Retraction Watch rows without `OriginalPaperDOI` or `OriginalPaperPubMedID`
  cannot match in this first pass.
- Weak-signal output is separate from the hard DOI/PMID output and should be
  reviewed as lower-confidence evidence.
- The hard output is per matched citation row; use `--article-dedup` or the
  standard-error duplicate counters for article-level analysis.

## SEE ALSO

wp-cite-extract, wp-download
