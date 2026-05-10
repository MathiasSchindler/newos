# IMGINFO

## NAME

imginfo - show basic image metadata

## SYNOPSIS

```
imginfo [-m|--mime] [-p|--plain] [-d|--details] [file ...]
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
- TIFF, classic TIFF headers and first image directory
- WebP, including VP8, VP8L, and VP8X headers
- BMP

## OPTIONS

- `-m`, `--mime` - print only the detected MIME type for each input
- `-p`, `--plain` - print tab-separated fields for scripts:
  `path format width height bit-depth channels mime`
- `-d`, `--details` - print one labeled metadata block per input, including
  variant, color model, compression, density, orientation, frame count, and
  properties when known
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
  properties: alpha
```

Properties may include `alpha`, `palette`, `interlaced`, `animated`,
`progressive`, `lossless`, `exif`, `icc-profile`, `xmp`, `top-down`,
`looping`, and `orientation`.

## LIMITATIONS

- JPEG dimensions are found by scanning the beginning of the file for a SOF marker; unusual files with very large metadata prefixes may report the format without dimensions.
- TIFF support is limited to classic TIFF, not BigTIFF.
- Animated frame counts are reported only for formats whose lightweight header
  metadata exposes them within the probe window.
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
cat image.webp | imginfo
```

## SEE ALSO

imgcheck, file, hexdump
