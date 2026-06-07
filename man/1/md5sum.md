# MD5SUM

## NAME

md5sum - compute or verify MD5 checksums

## SYNOPSIS

```
md5sum [-c] [-q] [-s] [-z] [FILE ...]
```

## DESCRIPTION

The md5sum tool computes MD5 digests for files or standard input and can verify checksum lists in check mode.

## CURRENT CAPABILITIES

- compute hashes for files or standard input
- verify checksum manifests with `-c`
- reduce output with quiet and status modes
- support NUL-delimited records with `-z`

## OPTIONS

| Flag | Description |
|------|-------------|
| `-c`, `--check` | Read checksum lines from a file and verify them. |
| `-q`, `--quiet` | Suppress `OK` lines for verified files. |
| `-s`, `--status` | Produce no output; use the exit status only. |
| `-z`, `--zero` | End output lines with NUL instead of newline. |

## LIMITATIONS

- MD5 is not cryptographically secure; prefer `sha256sum` or `sha512sum` for security-sensitive uses.
- BSD-style `--tag` output is not implemented.
- Check-file parsing is focused on standard `md5sum` lines; escaped filenames,
  binary/text mode markers, and GNU warning modes are limited.
- No recursive directory mode or automatic manifest discovery is provided.

## EXAMPLES

```
md5sum file.iso
md5sum -c checksums.txt
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

sha1sum, sha256sum, sha512sum, wc
