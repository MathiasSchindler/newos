# CMP

## NAME

cmp - compare two files byte by byte

## SYNOPSIS

```
cmp [-s] [-l] [-i SKIP1[:SKIP2]] [-n LIMIT] file1 file2
```

## DESCRIPTION

`cmp` compares FILE1 and FILE2 byte by byte. By default it reports the first
position where they differ and exits 0 if identical, 1 if different.

## CURRENT CAPABILITIES

- Report first differing byte position and line number (default mode)
- Silent mode (exit code only) with `-s`
- List all differing byte positions with `-l`
- Skip a number of bytes at the start of each file with `-i`
- Limit comparison to N bytes with `-n`

## OPTIONS

- `-s` — silent; suppress output, report result via exit code only
- `-l` — list all differing bytes: offset, FILE1 octal value, FILE2 octal value
- `-i SKIP1[:SKIP2]` — skip SKIP1 bytes of FILE1 and SKIP2 bytes of FILE2
  before comparing (SKIP2 defaults to SKIP1 if omitted)
- `-n LIMIT` — compare at most LIMIT bytes

## LIMITATIONS

- `-l` and `-s` cannot be combined.
- No verbose (`-v`) flag.

## EXAMPLES

```
cmp file1.bin file2.bin
cmp -s a.out b.out && echo "identical"
cmp -l image1.iso image2.iso | head
cmp -i 512 disk1.img disk2.img
```

## SEE ALSO

diff, hexdump
