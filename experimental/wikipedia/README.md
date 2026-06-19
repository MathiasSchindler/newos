# Wikipedia Tools

This directory tracks the experimental Wikipedia/MediaWiki tool work. The tools
themselves should still use the normal project layout when they are ordinary
commands: public entry points in `src/tools/`, shared reusable components in
`src/shared/`, and tool-private modules under `src/tools/<tool>/` if a command
outgrows one file.

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
