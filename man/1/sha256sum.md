# SHA256SUM

## NAME

sha256sum - compute or verify SHA-256 checksums

## SYNOPSIS

```
sha256sum [-c] [-q] [-s] [-z] [FILE ...]
```

## DESCRIPTION

The sha256sum tool computes SHA-256 digests for files or standard input and can verify checksum manifests.

## CURRENT CAPABILITIES

- compute SHA-256 hashes for files or standard input
- verify checksum manifests with `-c`
- support quiet and status-only verification modes
- emit NUL-delimited records with `-z`

## OPTIONS

| Flag | Description |
|------|-------------|
| `-c`, `--check` | Read checksum lines from a file and verify them. |
| `-q`, `--quiet` | Suppress `OK` lines for verified files. |
| `-s`, `--status` | Produce no output; use the exit status only. |
| `-z`, `--zero` | End output lines with NUL instead of newline. |

## LIMITATIONS

- BSD-style `--tag` output is not implemented.

## EXAMPLES

```
sha256sum image.tar
sha256sum -c checksums.sha256
```

## SEE ALSO

md5sum, sha512sum
