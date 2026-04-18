# SHA512SUM

## NAME

sha512sum - compute or verify SHA-512 checksums

## SYNOPSIS

sha512sum [-c] [-q] [-s] [-z] [FILE ...]

## DESCRIPTION

The sha512sum tool computes SHA-512 digests for files or standard input and verifies checksum manifests in check mode.

## CURRENT CAPABILITIES

- compute SHA-512 hashes for files or standard input
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

- `sha512sum backup.tar`
- `sha512sum -c checksums.sha512`

## SEE ALSO

md5sum, sha256sum
