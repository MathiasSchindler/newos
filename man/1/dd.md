# DD

## NAME

dd - copy and convert data in fixed-size blocks

## SYNOPSIS

```
dd [if=FILE] [of=FILE] [bs=N] [count=N] [skip=N] [seek=N]
   [conv=KEYWORD,...] [status=default|noxfer|none]
```

## DESCRIPTION

`dd` copies data from an input file (or stdin) to an output file (or stdout)
in blocks of a specified size, with optional skip and seek offsets. It is
commonly used for low-level copying, device I/O, and data extraction.

## CURRENT CAPABILITIES

- Block-based copy with configurable block size (`bs`)
- Skip input blocks with `skip`; seek into output with `seek`
- Limit transfer to `count` input blocks
- Conversion options: `sync` (pad short blocks with NUL), `noerror` (continue
  on read errors), `notrunc` (do not truncate output file)
- Status reporting: `default`, `noxfer` (suppress transfer stats), `none`
  (suppress all stats)

## OPTIONS

- `if=FILE` — read from FILE (default: stdin)
- `of=FILE` — write to FILE (default: stdout)
- `bs=N` — block size in bytes (default: 512)
- `count=N` — copy at most N input blocks
- `skip=N` — skip N input blocks before reading
- `seek=N` — skip N output blocks before writing
- `conv=KEYWORD,...` — apply conversions; supported: `sync`, `noerror`, `notrunc`
- `status=VALUE` — control progress reporting: `default`, `noxfer`, `none`

## LIMITATIONS

- No `ibs`/`obs` (separate input/output block sizes).
- `conv=` keywords beyond `sync`, `noerror`, `notrunc` are not supported.
- No progress display during transfer (only a final summary).

## EXAMPLES

```
dd if=/dev/zero of=blank.img bs=1024 count=100
dd if=disk.img of=/dev/sda bs=4096
dd if=file.bin bs=1 skip=128 count=64 | hexdump
dd if=input.dat of=output.dat conv=noerror,sync
```

## SEE ALSO

hexdump, od, cp
