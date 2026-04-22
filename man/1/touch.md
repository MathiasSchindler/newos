# TOUCH

## NAME

touch - update file timestamps and create empty files

## SYNOPSIS

```text
touch [-acm] [--no-create]
      [-d DATETIME | --date[=DATETIME] | -t STAMP]
      [-r FILE | --reference[=FILE]] [--time=WORD] path ...
```

## DESCRIPTION

`touch` updates access and modification times and can create files that do not
yet exist.

## CURRENT CAPABILITIES

- create missing files
- update access and modification times
- change only access time with `-a`
- change only modification time with `-m`
- set time from a literal value or a reference file
- accept GNU-style long aliases such as `--date`, `--reference`, `--time`, and `--no-create`

## OPTIONS

- `-a` update only access time
- `-c`, `--no-create` do not create missing files
- `-m` update only modification time
- `-d DATETIME`, `--date[=DATETIME]` choose a literal timestamp, including `@EPOCH`
- `-t STAMP` use a fixed POSIX-style touch timestamp
- `-r FILE`, `--reference[=FILE]` copy timestamps from another file
- `--time=WORD` choose `access`/`atime`/`use` or `modify`/`mtime`

## LIMITATIONS

- time parsing is focused on the supported project formats
- nanosecond-level or locale-specific host behavior is not the main target

## EXAMPLES

```text
touch file.txt
touch -r template.txt copy.txt
touch -t 202604182300 note.txt
```

## SEE ALSO

stat, date, mkdir
