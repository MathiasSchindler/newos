# DD

## NAME

dd - copy and convert data in fixed-size blocks

## SYNOPSIS

```
dd [if=FILE] [of=FILE] [bs=N] [ibs=N] [obs=N]
   [count=N] [skip=N] [seek=N]
   [conv=KEYWORD,...] [status=default|progress|noxfer|none]
```

## DESCRIPTION

`dd` copies data from an input file (or stdin) to an output file (or stdout)
in blocks of a specified size, with optional skip and seek offsets. It is
commonly used for low-level copying, device I/O, and data extraction.

## CURRENT CAPABILITIES

- Block-based copy with configurable block size (`bs`)
- Separate input and output block sizes with `ibs` / `obs`
- Skip input blocks with `skip`; seek into output with `seek`
- Limit transfer to `count` input blocks
- Conversion options: `sync` (pad short blocks with NUL), `noerror` (continue
  on read errors), `notrunc` (do not truncate output file)
- Status reporting: `default`, `progress`, `noxfer` (suppress transfer stats), `none`
  (suppress all stats)
- Size arguments accept common suffixes such as `k`, `M`, `G`, `w`, and `b`
- `count`, `skip`, and `seek` operate in units of the current input or output block size

## OPTIONS

- `if=FILE` — read from FILE (default: stdin)
- `of=FILE` — write to FILE (default: stdout)
- `bs=N` — set both input and output block sizes to N bytes (default: 512)
- `ibs=N` — input block size in bytes
- `obs=N` — output block size in bytes
- `count=N` — copy at most N input blocks
- `skip=N` — skip N input blocks before reading
- `seek=N` — skip N output blocks before writing
- `conv=KEYWORD,...` — apply conversions; supported: `sync`, `noerror`, `notrunc`
- `status=VALUE` — control progress reporting: `default`, `progress`, `noxfer`, `none`

## LIMITATIONS

- `conv=` keywords beyond `sync`, `noerror`, `notrunc` are not supported.
- No `iflag=...` or `oflag=...` support yet.

## EXAMPLES

```
dd if=/dev/zero of=blank.img bs=1024 count=100
dd if=input.bin of=output.bin ibs=4k obs=64k status=progress
dd if=disk.img of=/dev/sda bs=4096
dd if=file.bin bs=1 skip=128 count=64 | hexdump
dd if=input.dat of=output.dat conv=noerror,sync
```

## SEE ALSO

hexdump, od, cp
