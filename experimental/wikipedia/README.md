# Wikipedia Tools

This directory tracks the experimental Wikipedia/MediaWiki tool work. The tools
themselves live in this directory and build into `experimental/wikipedia/build`
so niche MediaWiki experiments stay out of the official tool list. Reusable
components should still move into `src/shared/` when they become generally
useful across multiple tools.

## wp-download

`wp-download` downloads MediaWiki XML bzip2 snapshots from
`https://dumps.wikimedia.org/other/mediawiki_content_current/`, using the
language edition code as input. For example:

```sh
wp-download fur
wp-download --date 2026-06-01 -o experimental/wikipedia/data fur
```

The tool discovers the latest snapshot date unless `--date YYYY-MM-DD` is
provided, reads the `xml/bzip2/SHA256SUMS` manifest, downloads every matching
`<lang>wiki-*.xml.bz2` file, reports progress with speed and ETA where the
directory listing provides file sizes, and verifies the SHA-256 digest while
streaming the file to disk. Progress lines are timestamped and include `file
N/TOTAL` for multi-file snapshots plus both file-level and package-level ETA
when size information is available.

Downloads are sequential for now. Wikimedia's own downloads front page says
their servers cap clients to 3 per-IP connections and that clients trying to
evade those limits may be blocked; mirrors may have different limits. If
parallel downloads are added later, keep the default conservative and make any
concurrency limit explicit.

Resume support is not implemented yet. A failed or interrupted file is
downloaded again from the beginning on the next run.

## wp-cite-extract

`wp-cite-extract` scans MediaWiki XML dumps and writes a TSV citation cache for
later matching against sources such as Retraction Watch. By default it looks in
`experimental/wikipedia/data` when run from the repository root, falls back to
`data` when run from this directory, chooses the newest `*wiki-YYYY-MM-DD-*.xml`
or `*.xml.bz2` snapshot set, and writes `<wiki>-<date>-citations.tsv` into the
same data directory.

```sh
experimental/wikipedia/build/wp-cite-extract
experimental/wikipedia/build/wp-cite-extract -i experimental/wikipedia/data/dewiki-2026-06-01-p1p3456636.xml.bz2
experimental/wikipedia/build/wp-cite-extract -o experimental/wikipedia/data/dewiki-citations.tsv
```

The first pass is heuristic and citation-cache oriented. It scans namespace 0
pages for raw DOI, PMID, ISBN, ISSN, German ISBN signals such as `978-3` and
`9783`, `<ref>` blocks, and common citation templates such as `{{Literatur}}`,
`{{Internetquelle}}`, and `{{Cite...}}`. The output columns are:

```text
wiki	snapshot	source	page_id	page_title	kind	value	raw
```

The binary remains statically linked and libc-free. `.xml.bz2` input is decoded
through the in-tree shared bzip2 decoder; no external `bzip2` or Python helper
is needed for extraction.

## wp-retraction-match

`wp-retraction-match` streams a citation TSV and matches DOI/PMID identifier rows
against `OriginalPaperDOI` and `OriginalPaperPubMedID` in the current Retraction
Watch CSV export:

```sh
experimental/wikipedia/build/wp-retraction-match
experimental/wikipedia/build/wp-retraction-match -c experimental/wikipedia/data/dewiki-2026-06-01-citations.tsv
experimental/wikipedia/build/wp-retraction-match -o experimental/wikipedia/data/dewiki-retraction-matches.tsv
experimental/wikipedia/build/wp-retraction-match -w experimental/wikipedia/data/dewiki-weak-retraction-matches.tsv
experimental/wikipedia/build/wp-retraction-match --article-dedup experimental/wikipedia/data/dewiki-retraction-matches.article.tsv
```

By default it uses `experimental/wikipedia/data/retraction_watch.csv`, chooses the
newest `*wiki-YYYY-MM-DD-citations.tsv` in the same data directory, and writes
`<wiki>-<date>-retraction-matches.tsv`. It also writes conservative weak title
matches to `<wiki>-<date>-weak-retraction-matches.tsv` unless `--no-weak` is
used. The matcher indexes Retraction Watch DOI/PMID keys in a hash table, indexes
weak title anchors separately, then streams the multi-GB citation TSV without
loading it all at once. Rows that are not DOI/PMID identifier rows or weak
`template`/`ref` candidates are skipped after inspecting the row prefix, avoiding
full TSV parsing for irrelevant citation kinds. The hard DOI/PMID output stays
row-level so raw evidence is preserved; weak title/journal/year/author-token
evidence is kept in the separate weak output for review. Non-quiet runs report
processed-row, identifier-row, lookup, hard-match, weak-match, and article-level
duplicate counters split by hard DOI vs PMID, and
`--article-dedup` can write a separate article-level hard-match summary. Weak
matching is future-work review evidence: it misses paraphrased, translated,
non-ASCII-only, or context-poor citations and can still produce false positives.

`wp-retraction-report.py` turns the exact and weak TSV outputs into a static
review page under `experimental/wikipedia/data/retraction/`:

```sh
python3 experimental/wikipedia/wp-retraction-report.py
```

The generated `index.html` links to external `retraction.css` and
`retraction.js`, provides sortable/filterable exact and weak tables, links each
row to the Wikipedia article, and links DOI/PMID rows to the published article.
