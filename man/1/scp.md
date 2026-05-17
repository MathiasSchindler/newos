# scp(1)

## Name

scp - copy files using scp-style operands

## Synopsis

`scp [-r] [-p] [-P PORT] [-i IDENTITY] SOURCE... DEST`

## Description

`scp` copies files using familiar scp command-line syntax.

The current implementation supports local-to-local copies. Remote operands of the form `HOST:PATH` or `USER@HOST:PATH` are detected and rejected with a clear diagnostic until the in-tree SSH layer exposes command execution or SFTP/scp channels.

## Options

- `-r` copy directories recursively
- `-p` preserve mode and symlink policy where supported by the shared copy helper
- `-P PORT` accept a remote SSH port for compatibility; remote transfer is not implemented yet
- `-i IDENTITY` accept an identity path for compatibility; remote transfer is not implemented yet
- `-h`, `--help` show usage information

## Examples

`scp file.txt copy.txt`

`scp -r src backup-src`

## Exit Status

Returns 0 when all requested local copies succeed. Returns non-zero for copy failures, invalid arguments, or remote operands.