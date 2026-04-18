# TOUCH

## NAME

touch - update file timestamps and create empty files

## SYNOPSIS

```text
touch [-acm] [-d DATETIME | -t STAMP | -r FILE] path ...
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

## OPTIONS

- `-a` update only access time
- `-c` do not create missing files
- `-m` update only modification time
- `-d DATETIME`, `-t STAMP`, `-r FILE` choose the source timestamp

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
