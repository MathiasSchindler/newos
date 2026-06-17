# ZIP

## NAME

zip - create ZIP archives

## SYNOPSIS

```
zip [-0|-1..-9] [-r] [-v] ARCHIVE FILE ...
```

## DESCRIPTION

`zip` creates a ZIP archive from the named files and directories. It writes a
normal central directory and stores directory entries when directories are
added. File entries are deflated by default when the result is smaller, and are
stored otherwise.

## OPTIONS

- `-0` - store entries without compression.
- `-1` through `-9` - choose faster or stronger deflate compression. The
	default is `-6`. Higher levels search more matches and may use dynamic Huffman
	blocks when they help.
- `-r` - recursively add directory contents.
- `-v` - print entries as they are added.
- `-h`, `--help` - show usage.

## JSON Output

This command does not provide a JSON output mode.
JSON mode limitation: no JSON output mode is available.

## LIMITATIONS

Updating existing archives, deleting archive members, comments, split archives,
encryption, external attributes, ZIP64 output, and block splitting are future
work. Archives larger than classic ZIP limits are rejected. Deflate output is
project-local and still simpler than mature ZIP implementations, but supports
level-dependent match search, lazy matching at stronger levels, and dynamic
Huffman blocks at higher levels.

## SEE ALSO

unzip, tar, gzip, readapk
