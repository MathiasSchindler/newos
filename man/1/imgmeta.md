# IMGMETA

## NAME

imgmeta - show, edit, copy, and strip image metadata

## SYNOPSIS

```
imgmeta show [file ...]
imgmeta list-text [file ...]
imgmeta strip -o OUTPUT FILE
imgmeta copy -o OUTPUT FILE
imgmeta edit [--set-text KEY=VALUE|--remove-text KEY] -o OUTPUT FILE
```

## DESCRIPTION

`imgmeta` works with metadata carried by supported image containers. It can show metadata-related properties reported by the shared image probe layer, list PNG text metadata, write a metadata-stripped copy for PNG, JPEG, and WebP inputs, edit PNG text metadata, or copy a recognized image while preserving its bytes and metadata.

The command does not decode pixels. Metadata stripping rewrites container structure only, preserving image data and unsupported segments where practical.

When `show` is used without a file, `imgmeta` reads from standard input. `strip`, `copy`, and `edit` currently require an explicit output path.

## COMMANDS

- `show` - print detected image format, metadata properties, orientation, and density when available
- `list-text` - print tab-separated PNG text metadata records as `path type key value`; compressed text chunks are listed by key without decompression
- `strip -o OUTPUT FILE` - write FILE to OUTPUT without supported metadata chunks or segments
- `copy -o OUTPUT FILE` - write a recognized image to OUTPUT while preserving the original bytes and metadata
- `edit --set-text KEY=VALUE -o OUTPUT FILE` - write a PNG copy with a `tEXt` metadata entry inserted or replaced
- `edit --remove-text KEY -o OUTPUT FILE` - write a PNG copy with matching `tEXt` metadata entries removed

## STRIP BEHAVIOR

For PNG, `strip` removes metadata chunks such as `eXIf`, `iCCP`, `iTXt`, `tEXt`, `zTXt`, and `tIME`. Critical chunks and image data are preserved.

For JPEG, `strip` removes EXIF APP1 segments, XMP APP1 segments, ICC APP2 segments, and COM comment segments before the scan data. The entropy-coded image stream is copied unchanged.

For WebP, `strip` removes `EXIF`, `XMP `, and `ICCP` RIFF chunks and clears the corresponding extended WebP feature flags. Image chunks are copied unchanged.

## EDIT BEHAVIOR

For PNG, `edit --set-text KEY=VALUE` inserts or replaces an uncompressed `tEXt` chunk before image data. The new chunk is written with a fresh CRC. Existing critical chunks and image data are copied unchanged.

For PNG, `edit --remove-text KEY` removes matching uncompressed `tEXt` chunks while preserving image data and other chunks.

## LIMITATIONS

- `strip` is implemented for PNG, JPEG, and WebP only.
- TIFF metadata stripping is not implemented yet; TIFF IFD rewriting is still pending.
- Metadata editing is currently limited to PNG `tEXt` key/value entries. `iTXt` and `zTXt` chunks can be listed by key, but are not decompressed or edited yet.
- Language-tagged or compressed PNG `iTXt` editing is not implemented yet.
- Metadata copying is currently whole-image copying; selective transfer between two different image files is not implemented yet.
- PNG stripping preserves existing chunk CRCs for retained chunks and does not recompress image data.
- JPEG stripping is segment-oriented and does not parse entropy-coded scan data.
- Color-management metadata such as ICC profiles may affect visual interpretation; stripping it can change how other software displays the same pixels.

## EXAMPLES

```
imgmeta show photo.jpg
imgmeta show image.png image.gif
imgmeta strip -o clean.png image.png
imgmeta strip -o clean.jpg photo.jpg
imgmeta strip -o clean.webp image.webp
imgmeta copy -o preserved.jpg photo.jpg
imgmeta list-text image.png
imgmeta edit --set-text comment=reviewed -o edited.png image.png
imgmeta edit --remove-text comment -o clean-text.png edited.png
```

## SEE ALSO

imginfo, imgcheck, file
