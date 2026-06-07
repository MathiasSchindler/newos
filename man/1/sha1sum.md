# SHA1SUM

## NAME

sha1sum - compute or verify SHA-1 checksums

## SYNOPSIS

```
sha1sum [-bct] [-q] [-s] [-z] [FILE ...]
```

## DESCRIPTION

The sha1sum tool computes SHA-1 digests for files or standard input and can verify checksum manifests.

SHA-1 is no longer suitable for cryptographic trust decisions, but it remains useful for compatibility with historical manifests and for non-adversarial transfer checks.

## CURRENT CAPABILITIES

- compute SHA-1 hashes for files or standard input
- verify checksum manifests with `-c`
- emit text or binary checksum markers
- support quiet and status-only verification modes
- emit NUL-delimited records with `-z`

## OPTIONS

| Flag | Description |
|------|-------------|
| `-b`, `--binary` | Emit binary-mode checksum records using `*FILE`. |
| `-c`, `--check` | Read checksum lines from a file and verify them. |
| `-q`, `--quiet` | Suppress `OK` lines for verified files. |
| `-s`, `--status` | Produce no output; use the exit status only. |
| `-t`, `--text` | Emit text-mode checksum records using two spaces before `FILE`. |
| `-z`, `--zero` | End output lines with NUL instead of newline. |

## LIMITATIONS

- BSD-style `--tag` output is not implemented.
- Check-file parsing is focused on standard `sha1sum` lines; escaped
  filenames, binary/text mode markers, and GNU warning modes are limited.
- No recursive directory mode or automatic manifest discovery is provided.

## EXAMPLES

```
sha1sum image.tar
sha1sum -c checksums.sha1
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

md5sum, sha256sum, sha512sum