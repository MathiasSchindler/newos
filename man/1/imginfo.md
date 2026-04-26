# IMGINFO

## NAME

imginfo - show basic image metadata

## SYNOPSIS

```
imginfo [-m|--mime] [-p|--plain] [file ...]
```

## DESCRIPTION

`imginfo` probes image files and prints their format, dimensions, bit depth,
channel information, and MIME type when that metadata is available from the
file header.

The command is intentionally metadata-only. It does not decode pixels, allocate
large image buffers, or validate full image contents.

When no file is provided, `imginfo` reads from standard input.

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
- `-h`, `--help` - show usage

Unknown numeric fields are printed as `-` in plain output.

## LIMITATIONS

- JPEG dimensions are found by scanning the beginning of the file for a SOF marker; unusual files with very large metadata prefixes may report the format without dimensions.
- TIFF support is limited to classic TIFF, not BigTIFF.
- Animated image frame counts are not currently reported.
- The command does not perform full image validation or decompression.

## EXAMPLES

```
imginfo picture.png
imginfo --plain *.jpg
cat image.webp | imginfo
```

## SEE ALSO

file, hexdump
