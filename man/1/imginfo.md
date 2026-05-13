# IMGINFO

## NAME

imginfo - show basic image metadata

## SYNOPSIS

```
imginfo [-m|--mime] [-p|--plain] [-d|--details] [--json] [--c2pa-trust] [--canonical-ext] [-R|--recursive] [file ...]
```

## DESCRIPTION

`imginfo` probes image files and prints their format, dimensions, bit depth,
channel information, color model, compression family, feature properties, and
MIME type when that metadata is available from the file header. It also reports
embedded C2PA/JUMBF manifest-store carriers when they are visible in supported
image containers.

The command is intentionally metadata-only. It does not decode pixels, allocate
large image buffers, or validate full image contents.

When no file is provided, `imginfo` reads from standard input.

If an input path has a filename extension that does not match the detected image
format, `imginfo` prints a warning to standard error. The warning does not
change the exit status and does not alter standard output.

## SUPPORTED FORMATS

- PNG
- JPEG
- GIF
- TIFF, including classic TIFF and BigTIFF headers with the first image directory
- WebP, including VP8, VP8L, and VP8X headers
- BMP

## OPTIONS

- `-m`, `--mime` - print only the detected MIME type for each input
- `-p`, `--plain` - print tab-separated fields for scripts:
  `path format width height bit-depth channels mime`
- `-d`, `--details` - print one labeled metadata block per input, including
  variant, color model, compression, density, orientation, frame count,
  animation duration, loop count, and properties when known
- `--json` - print one JSON object per input, including the canonical extension
  and known metadata fields
- `--c2pa-trust`, `--trust` - enable conservative C2PA trust-policy reporting
  based on verified signatures, parseable embedded certificates, explicit
  content-hash mismatches, embedded validation failures, and unsupported
  timestamp trust checks
- `--canonical-ext` - print only the detected canonical extension for each input
- `-R`, `--recursive` - walk directory operands recursively
- `-h`, `--help` - show usage

Unknown numeric fields are printed as `-` in plain output.

## OUTPUT

The default output is a compact one-line summary. `--details` prints a more
stable labeled form for humans:

```
picture.png:
  format: PNG
  extension: png
  mime: image/png
  variant: PNG
  dimensions: 640x480
  bit-depth: 8
  channels: 4 (rgba)
  color: truecolor-alpha
  compression: deflate
  density: 3780x3780 pixels/meter
  orientation: -
  frames: -
  duration-ms: -
  loop-count: -
  properties: alpha
  c2pa: -
```

Properties may include `alpha`, `palette`, `interlaced`, `animated`,
`progressive`, `lossless`, `exif`, `icc-profile`, `xmp`, `top-down`,
`looping`, `orientation`, and `c2pa`.

When C2PA metadata is present, `--details` prints a structural summary with the
carrier, signature algorithm, JUMBF box count, CBOR box count, manifest count,
claim count, assertion-store count, COSE signature count, verified/invalid
signature counts, X.509 certificate count, embedded validation-failure count,
content-hash status, signature-verification status, trust-validation status,
and ingredient count. JSON output includes the same summary in a `c2pa` object.

## LIMITATIONS

- JPEG dimensions are found by scanning for a SOF marker. Path inputs that look like JPEGs but do not expose dimensions in the initial probe window are retried with a full-file probe; standard input is read fully for the same reason.
- TIFF support reads classic TIFF and BigTIFF first image directories, but does not follow nested IFD trees or offsets beyond the local addressable range.
- Animated frame counts, total duration, and loop counts are reported for APNG, GIF, and WebP when those lightweight container records are visible within the probe window.
- Deeper animation metadata, such as per-frame disposal/blend behavior, is not reported.
- The command does not perform full image validation or decompression.
- Metadata discovery is intentionally shallow; EXIF payloads, ICC profiles, XMP
  packets, PNG text chunks, and nested TIFF tag directories are detected or
  summarized rather than fully decoded into individual fields.
- C2PA support parses JUMBF boxes, validates definite-length CBOR structure,
  recognizes COSE_Sign1 signatures, reports embedded X.509 certificate blobs,
  and attempts SHA-256 content-hash checks for visible hash-data bindings.
  ES256/P-256 COSE signatures can be verified against embedded P-256 leaf
  certificates. With `--c2pa-trust`, `imginfo` runs a conservative policy check:
  explicit hash mismatches, invalid signatures, malformed certificate blobs,
  embedded validation failures, and timestamped manifests without implemented
  timestamp validation report as untrusted. External trust anchors, certificate
  path building, TSA validation, and full claim conformance are not implemented
  yet.
- EXIF orientation is read from the first TIFF-style image file directory only;
  maker notes and deeper metadata trees are not interpreted.
- Filename-extension checks are heuristic. Extension mismatches are warnings
  only, and files without an extension are not treated as errors.

## EXAMPLES

```
imginfo picture.png
imginfo --details picture.png
imginfo --plain *.jpg
imginfo --json --recursive images/
imginfo --canonical-ext picture.png
cat image.webp | imginfo
```

## SEE ALSO

imgcheck, imgmeta, file, hexdump
