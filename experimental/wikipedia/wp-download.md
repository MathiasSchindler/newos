# WP-DOWNLOAD

## NAME

wp-download - download Wikipedia MediaWiki XML bzip2 snapshots

## SYNOPSIS

```
wp-download [-q] [-o DIR] [--date YYYY-MM-DD] [-T TIMEOUT] [--retries N] [--jobs N] [--no-resume] LANG
```

## DESCRIPTION

`wp-download` retrieves Wikipedia MediaWiki content dumps from Wikimedia's
current content snapshot service. `LANG` is a language edition prefix such as
`fur` or `de`; the tool maps it to the corresponding wiki name such as
`furwiki` or `dewiki`.

By default it finds the latest available snapshot date, reads the snapshot's
`xml/bzip2/SHA256SUMS` manifest, downloads every matching `*.xml.bz2` dump file,
and verifies each file's SHA-256 digest while streaming it to disk. Progress is
reported to standard error unless quiet mode is enabled. Progress lines include
timestamps, `file N/TOTAL` for multi-file snapshots, transferred bytes, average
speed, file ETA when the response provides `Content-Length`, and total package
progress/ETA when the directory listing provides file sizes.

## OPTIONS

- `-q` / `--quiet` - suppress progress and status messages
- `-o DIR` / `--output-dir DIR` - write downloaded files into DIR; the directory
  must already exist. The default is the current directory.
- `--date YYYY-MM-DD` - download a specific snapshot date instead of discovering
  the latest one
- `-T TIMEOUT` / `--timeout TIMEOUT` - set an idle socket timeout; accepts the
  same duration syntax used by other tools such as `250ms`, `2s`, or `1.5m`.
  The default is 30 seconds.
- `--retries N` - retry a failed file download up to N times after the first
  attempt. The default is 3 retries; the maximum is 100.
- `-j N` / `--jobs N` - download up to N dump files concurrently. The default
  is 1. Values above 3 are rejected to stay within Wikimedia's published
  per-IP connection limit.
- `--no-resume` - ignore any partial output file and restart each selected file
  from byte 0.

## EXAMPLES

```
wp-download fur
wp-download --date 2026-06-01 -o dumps fur
wp-download -q --date 2026-06-01 de
wp-download --jobs 3 --retries 5 --date 2026-06-01 de
```

## LIMITATIONS

- The initial implementation supports the Wikimedia `mediawiki_content_current`
  bzip2 XML layout only.
- Output directories are not created automatically.
- Proxy support and mirror selection are not implemented.
- Large language editions may produce multiple bzip2 XML files; all matching
  files covered by `SHA256SUMS` are downloaded.

## DOWNLOAD ETIQUETTE

Wikimedia's dumps front page says Wikimedia servers cap downloaders to 3 per-IP
connections and that clients trying to evade those limits may be blocked. This
tool downloads sequentially by default. `--jobs` is capped at 3, and users doing
large regular downloads should still prefer mirrors or torrents where available.

The tool resumes partial files with HTTP `Range` requests by default, verifies
the final SHA-256 digest before accepting a file, and retries transient failures
with a short backoff. If a server ignores a range request, the file is restarted
from the beginning.

HTTP and SOCKS proxy support is intentionally not part of this experimental
tool. It is uncommon on contemporary developer networks and would add a separate
connection policy surface to a downloader whose main target is Wikimedia's
public dump service.

## SEE ALSO

wget, sha256sum, bzip2
