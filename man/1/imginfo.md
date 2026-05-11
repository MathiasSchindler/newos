# IMGINFO

## NAME

imginfo - show basic image metadata

## SYNOPSIS

```
imginfo [-m|--mime] [-p|--plain] [-d|--details] [--json] [--canonical-ext] [-R|--recursive] [file ...]
```

## DESCRIPTION

`imginfo` probes image files and prints their format, dimensions, bit depth,
channel information, color model, compression family, feature properties, and
MIME type when that metadata is available from the file header.

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
```

Properties may include `alpha`, `palette`, `interlaced`, `animated`,
`progressive`, `lossless`, `exif`, `icc-profile`, `xmp`, `top-down`,
`looping`, and `orientation`.

## LIMITATIONS

- JPEG dimensions are found by scanning for a SOF marker. Path inputs that look like JPEGs but do not expose dimensions in the initial probe window are retried with a full-file probe; standard-input probes remain bounded and may still report the format without dimensions for unusual files with very large metadata prefixes.
- TIFF support reads classic TIFF and BigTIFF first image directories, but does not follow nested IFD trees or offsets beyond the local addressable range.
- Animated frame counts are reported for PNG APNG and WebP `ANMF` chunks when
  those lightweight container records are visible within the probe window. WebP
  `ANIM` loop counts and `ANMF` frame durations are also reported when present.
- Deeper animation metadata, such as per-frame disposal/blend behavior and exact timing for formats that require decompression or full stream parsing, is not reported.
- The command does not perform full image validation or decompression.
- Metadata discovery is intentionally shallow; EXIF payloads, ICC profiles, XMP
  packets, PNG text chunks, and nested TIFF tag directories are detected or
  summarized rather than fully decoded into individual fields.
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
