# ZIP

## NAME

zip - create ZIP archives

## SYNOPSIS

```
zip [-0] [-r] [-v] ARCHIVE FILE ...
```

## DESCRIPTION

`zip` creates a ZIP archive from the named files and directories. The current
implementation writes interoperable stored entries with a normal central
directory. Directory entries are included when directories are added.

## OPTIONS

- `-0` - store entries without compression. This is currently the only supported
  mode and is accepted for compatibility.
- `-r` - recursively add directory contents.
- `-v` - print entries as they are added.
- `-h`, `--help` - show usage.

## LIMITATIONS

Deflate compression, updating existing archives, deleting archive members,
comments, split archives, encryption, and ZIP64 output are future work. Archives
larger than classic ZIP limits are rejected.

## SEE ALSO

unzip, tar, gzip, readapk
