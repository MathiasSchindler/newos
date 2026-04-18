# MKTEMP

## NAME

mktemp - create a temporary file or directory

## SYNOPSIS

mktemp [-d] [-u] [-p DIR] [-t PREFIX] [TEMPLATE]

## DESCRIPTION

mktemp creates a uniquely named temporary file or directory. The TEMPLATE must contain at least three trailing `X` characters which are replaced with random characters. If no template is given a default pattern is used.

## CURRENT CAPABILITIES

- create a temporary file and print its path
- create a temporary directory with `-d`
- dry-run mode that prints a name without creating anything
- custom base directory via `-p`
- legacy prefix-only template via `-t`

## OPTIONS

- `-d` — create a directory instead of a file
- `-u` — dry run: generate and print a name but do not create anything
- `-p DIR` — create the temporary entry under DIR rather than the default temp directory; also accepted as `--tmpdir`
- `-t PREFIX` — use PREFIX as a template prefix (legacy interface; a suffix of `XXXXXX` is appended automatically)

## LIMITATIONS

- the template may only contain `X` characters in a contiguous trailing run; non-trailing `X` patterns are not substituted
- no `-q` quiet flag

## EXAMPLES

- `mktemp` — create a temporary file in the default directory
- `mktemp -d` — create a temporary directory
- `mktemp -p /var/tmp myapp.XXXXXX` — create a file under `/var/tmp`
- `mktemp -u` — print a candidate name without creating it

## SEE ALSO

rm, mkdir
