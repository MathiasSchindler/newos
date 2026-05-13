# IMGCHECK

## NAME

imgcheck - validate image file structure

## SYNOPSIS

```
imgcheck [-q|--quiet] [-v|--verbose] [-p|--plain] [--json] [--strict] [--c2pa-trust] [-R|--recursive] [file ...]
```

## DESCRIPTION

`imgcheck` reads image files and reports whether each input is recognized and structurally valid according to the checks implemented by the shared image parser. When C2PA/JUMBF metadata is present, it reports C2PA structure, CBOR, COSE, certificate, content-hash, signature, and trust status alongside the normal image-container result.

The first validation passes perform real PNG, JPEG, GIF, TIFF, WebP, and BMP container checks. PNG checks include signature, required chunk order, IHDR fields, chunk lengths, CRC values, required IDAT data, and IEND termination. JPEG checks include marker sequencing, segment lengths, SOF dimensions and component tables, SOS presence, scan-data marker escaping, EOI termination, and trailing data. GIF checks include the header, logical screen descriptor, global and local color table bounds, extension blocks, image descriptors, image data sub-block termination, and trailer termination. TIFF checks include byte order, magic number, first IFD bounds, value offsets, and required first-image dimensions for classic TIFF. WebP checks include RIFF sizing, chunk bounds, and required image chunks. BMP checks include file and DIB headers, dimensions, plane count, bit depth, compression compatibility, color-table bounds, pixel-data offsets, and uncompressed pixel-array length. For uncompressed BMP files, the pixel-array span is verified against the decoded row layout.

When no file is provided, `imgcheck` reads from standard input.

## SUPPORTED FORMATS

- PNG, with structural chunk validation
- JPEG, with structural marker and segment validation
- GIF, with structural block validation
- BMP, with structural header, palette, and pixel-array validation
- TIFF, with classic TIFF and BigTIFF first-IFD structural validation
- WebP, with RIFF chunk validation

## OPTIONS

- `-q`, `--quiet` - suppress output and use only the exit status
- `-v`, `--verbose` - include the validation message for successful inputs
- `-p`, `--plain` - print tab-separated fields for scripts: `path format status failure-offset message`, with an additional C2PA status field when C2PA metadata is present
- `--json` - print one JSON object per input with `path`, `format`, `valid`, `status`, `message`, `failure_offset`, and a `c2pa` object
- `--strict` - reject additional spec-discouraged constructs, currently including ancillary PNG chunks after IDAT
- `--c2pa-trust`, `--trust` - enable conservative C2PA trust-policy reporting;
	explicit C2PA hash mismatches, invalid C2PA signatures, embedded validation
	failures cause a non-zero check result
- `-R`, `--recursive` - walk directory operands recursively
- `-h`, `--help` - show usage

When a validator can identify the byte position of the first failure, human,
plain, and JSON output include that zero-based byte offset. If no precise offset
is available, plain output prints `-` and JSON prints `null`.

## EXIT STATUS

`imgcheck` exits with status 0 when all inputs pass their available checks. It exits with status 1 when any input cannot be read, is unsupported, or fails validation.

## LIMITATIONS

- PNG, JPEG, GIF, TIFF, and WebP validation is structural and does not inflate, decompress, or verify decoded pixel data. BMP validation verifies uncompressed pixel-array bounds against the decoded row layout, but does not interpret color values.
- TIFF and BigTIFF validation currently check the first image file directory only; nested or chained IFD trees are not followed.
- `--strict` currently adds PNG ancillary-chunk ordering checks. Other strict policy checks will be added incrementally.
- Metadata payloads such as EXIF, ICC profiles, XMP packets, and textual chunks are not fully interpreted by this command.
- C2PA checking parses JUMBF boxes, validates definite-length CBOR structure,
	recognizes COSE_Sign1 signatures, reports embedded X.509 certificate blobs,
	and attempts SHA-256 content-hash checks for visible hash-data bindings.
	ES256/P-256 COSE signatures can be verified against embedded P-256 leaf
	certificates, and every supported COSE signature is counted as verified or
	invalid. With `--c2pa-trust`, `imgcheck` runs a conservative policy check:
	explicit hash mismatches, invalid signatures, malformed certificate blobs,
	embedded validation failures, and timestamped manifests without implemented
	timestamp validation report as untrusted. External trust anchors,
	certificate path building, TSA validation, and full C2PA claim conformance
	are not implemented yet.
- Very large inputs are read into memory before validation.

## EXAMPLES

```
imgcheck picture.png
imgcheck --verbose *.png
imgcheck --plain image.png image.jpg
imgcheck --json --recursive images/
imgcheck --strict image.png
cat image.png | imgcheck -q
```

## SEE ALSO

imginfo, imgmeta, file, hexdump
