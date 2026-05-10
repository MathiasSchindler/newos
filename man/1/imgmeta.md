# IMGMETA

## NAME

imgmeta - show and strip image metadata

## SYNOPSIS

```
imgmeta show [file ...]
imgmeta strip -o OUTPUT FILE
```

## DESCRIPTION

`imgmeta` works with metadata carried by supported image containers. The initial implementation can show metadata-related properties reported by the shared image probe layer and can write a metadata-stripped copy for PNG and JPEG inputs.

The command does not decode pixels. Metadata stripping rewrites container structure only, preserving image data and unsupported segments where practical.

When `show` is used without a file, `imgmeta` reads from standard input. `strip` currently requires an explicit output path.

## COMMANDS

- `show` - print detected image format, metadata properties, orientation, and density when available
- `strip -o OUTPUT FILE` - write FILE to OUTPUT without supported metadata chunks or segments

## STRIP BEHAVIOR

For PNG, `strip` removes metadata chunks such as `eXIf`, `iCCP`, `iTXt`, `tEXt`, `zTXt`, and `tIME`. Critical chunks and image data are preserved.

For JPEG, `strip` removes EXIF APP1 segments, XMP APP1 segments, ICC APP2 segments, and COM comment segments before the scan data. The entropy-coded image stream is copied unchanged.

## LIMITATIONS

- `strip` is implemented for PNG and JPEG only.
- Metadata editing and metadata copying are not implemented yet.
- PNG stripping preserves existing chunk CRCs for retained chunks and does not recompress image data.
- JPEG stripping is segment-oriented and does not parse entropy-coded scan data.
- Color-management metadata such as ICC profiles may affect visual interpretation; stripping it can change how other software displays the same pixels.

## EXAMPLES

```
imgmeta show photo.jpg
imgmeta show image.png image.gif
imgmeta strip -o clean.png image.png
imgmeta strip -o clean.jpg photo.jpg
```

## SEE ALSO

imginfo, imgcheck, file
