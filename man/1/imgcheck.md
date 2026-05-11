# IMGCHECK

## NAME

imgcheck - validate image file structure

## SYNOPSIS

```
imgcheck [-q|--quiet] [-v|--verbose] [-p|--plain] [--json] [-R|--recursive] [file ...]
```

## DESCRIPTION

`imgcheck` reads image files and reports whether each input is recognized and structurally valid according to the checks implemented by the shared image parser.

The first validation passes perform real PNG, JPEG, GIF, and BMP container checks. PNG checks include signature, required chunk order, IHDR fields, chunk lengths, CRC values, required IDAT data, and IEND termination. JPEG checks include marker sequencing, segment lengths, SOF dimensions and component tables, SOS presence, scan-data marker escaping, EOI termination, and trailing data. GIF checks include the header, logical screen descriptor, global and local color table bounds, extension blocks, image descriptors, image data sub-block termination, and trailer termination. BMP checks include file and DIB headers, dimensions, plane count, bit depth, compression compatibility, color-table bounds, pixel-data offsets, and uncompressed pixel-array length. For uncompressed BMP files, the pixel-array span is verified against the decoded row layout. Other recognized image formats currently use the shared probe path and are reported as recognized while deeper validators are still being added.

When no file is provided, `imgcheck` reads from standard input.

## SUPPORTED FORMATS

- PNG, with structural chunk validation
- JPEG, with structural marker and segment validation
- GIF, with structural block validation
- BMP, with structural header, palette, and pixel-array validation
- TIFF and WebP, recognized through the shared image probe layer pending deeper validation

## OPTIONS

- `-q`, `--quiet` - suppress output and use only the exit status
- `-v`, `--verbose` - include the validation message for successful inputs
- `-p`, `--plain` - print tab-separated fields for scripts: `path format status failure-offset message`
- `--json` - print one JSON object per input with `path`, `format`, `valid`, `status`, `message`, and `failure_offset`
- `-R`, `--recursive` - walk directory operands recursively
- `-h`, `--help` - show usage

When a validator can identify the byte position of the first failure, human,
plain, and JSON output include that zero-based byte offset. If no precise offset
is available, plain output prints `-` and JSON prints `null`.

## EXIT STATUS

`imgcheck` exits with status 0 when all inputs pass their available checks. It exits with status 1 when any input cannot be read, is unsupported, or fails validation.

## LIMITATIONS

- PNG, JPEG, and GIF validation is structural and does not inflate, decompress, or verify decoded pixel data. BMP validation verifies uncompressed pixel-array bounds against the decoded row layout, but does not interpret color values.
- TIFF and WebP currently receive recognition-level checks only; format-specific structural validators will be added incrementally.
- There is not yet a `--strict` mode for rejecting spec-discouraged but widely tolerated constructs such as late ancillary chunks, uncommon chunk ordering, or trailing compatibility data.
- Metadata payloads such as EXIF, ICC profiles, XMP packets, and textual chunks are not fully interpreted by this command.
- Very large inputs are read into memory before validation.

## EXAMPLES

```
imgcheck picture.png
imgcheck --verbose *.png
imgcheck --plain image.png image.jpg
imgcheck --json --recursive images/
cat image.png | imgcheck -q
```

## SEE ALSO

imginfo, imgmeta, file, hexdump
