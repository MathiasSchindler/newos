# Wikipedia Tools

This directory tracks the experimental Wikipedia/MediaWiki tool work. The tools
themselves live in this directory and build into `experimental/wikipedia/build`
so niche MediaWiki experiments stay out of the official tool list. Reusable
components should still move into `src/shared/` when they become generally
useful across multiple tools.

Run `make` in this directory to build the three experimental tools into
`experimental/wikipedia/build/`. On local macOS/aarch64 this uses the project's
Mach-O linker path; on other hosts it keeps the Linux freestanding build path.

## wp-download

`wp-download` downloads MediaWiki XML bzip2 snapshots from
`https://dumps.wikimedia.org/other/mediawiki_content_current/`, using the
language edition code as input. For example:

```sh
wp-download fur
wp-download --date 2026-06-01 -o experimental/wikipedia/data fur
wp-download --base-url https://mirror.example/mediawiki_content_current/ fur
```

The tool discovers the latest snapshot date unless `--date YYYY-MM-DD` is
provided, reads the `xml/bzip2/SHA256SUMS` manifest, downloads every matching
`<lang>wiki-*.xml.bz2` file, reports progress with speed and ETA where the
directory listing provides file sizes, and verifies the SHA-256 digest while
streaming the file to disk. Progress lines are timestamped and include `file
N/TOTAL` for multi-file snapshots plus both file-level and package-level ETA
when size information is available.

Downloads are sequential by default. `--jobs N` enables parallel file downloads,
but values above 3 are rejected because Wikimedia's own dumps front page says
their servers cap clients to 3 per-IP connections and that clients trying to
evade those limits may be blocked; mirrors may have different limits. Failed or
interrupted files are resumed with HTTP `Range` requests by default and verified
against `SHA256SUMS` before being accepted. `--retries N` controls retry count,
and `--no-resume` forces a fresh download from byte 0. Successful verification
lines and the final completion summary use the shared ANSI color helpers when
color output is enabled.

Active downloads use `<name>.part`; only a complete, SHA-256-verified file is
data-synced and atomically renamed to its final name. Resume responses must
return a matching `Content-Range`, while a server that ignores `Range` and
returns `200` causes a safe restart from byte zero. HTTP bodies support exact
`Content-Length` framing and streaming chunked transfer encoding. Metadata and
dump redirects are followed up to a fixed limit. `-T` applies to plain HTTP
waits and to TLS handshake and record I/O; activity starts a fresh per-read
wait.

Parallel mode reaps children in completion order, so a completed short download
immediately frees a slot even when an earlier file is still running. TLS record,
AES-GCM, and SHA-256 objects are built with `-O2`; the rest of the experimental
tool tree retains the size-oriented `-Oz` default.

`make test` runs a local deterministic downloader server in addition to the
retraction matcher smoke test. The downloader suite covers chunked metadata and
dump bodies, metadata and file redirects, valid resume, servers that ignore a
range, malformed `Content-Range`, checksum retry, truncated metadata, stalled
bodies, atomic promotion, and completion-order slot reuse.

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
