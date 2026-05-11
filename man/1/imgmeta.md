# IMGMETA

## NAME

imgmeta - show, edit, copy, and strip image metadata

## SYNOPSIS

```
imgmeta show [file ...]
imgmeta list-text [file ...]
imgmeta strip -o OUTPUT FILE
imgmeta copy -o OUTPUT [--from METADATA_SOURCE] FILE
imgmeta edit [--set-text KEY=VALUE|--set-itxt KEY=VALUE|--remove-text KEY] [--language TAG] [--compressed] -o OUTPUT FILE
```

## DESCRIPTION

`imgmeta` works with metadata carried by supported image containers. It can show metadata-related properties reported by the shared image probe layer, list PNG text metadata, write a metadata-stripped copy for PNG, JPEG, WebP, and TIFF inputs, edit PNG text metadata, copy a recognized image while preserving its bytes and metadata, or copy PNG metadata chunks between PNG files.

The command does not decode pixels. Metadata stripping rewrites container structure only, preserving image data and unsupported segments where practical.

When `show` is used without a file, `imgmeta` reads from standard input. `strip`, `copy`, and `edit` currently require an explicit output path.

## COMMANDS

- `show` - print detected image format, metadata properties, orientation, and density when available
- `list-text` - print tab-separated PNG text metadata records as `path type key value`; compressed text chunks are listed by key without decompression
- `strip -o OUTPUT FILE` - write FILE to OUTPUT without supported metadata chunks or segments
- `copy -o OUTPUT FILE` - write a recognized image to OUTPUT while preserving the original bytes and metadata
- `copy --from METADATA_SOURCE -o OUTPUT FILE` - write FILE to OUTPUT with PNG metadata chunks copied from METADATA_SOURCE
- `edit --set-text KEY=VALUE -o OUTPUT FILE` - write a PNG copy with a `tEXt` metadata entry inserted or replaced
- `edit --set-itxt KEY=VALUE [--language TAG] [--compressed] -o OUTPUT FILE` - write a PNG copy with an `iTXt` metadata entry inserted or replaced
- `edit --remove-text KEY -o OUTPUT FILE` - write a PNG copy with matching text metadata entries removed

## STRIP BEHAVIOR

For PNG, `strip` removes metadata chunks such as `eXIf`, `iCCP`, `iTXt`, `tEXt`, `zTXt`, and `tIME`. Critical chunks and image data are preserved.

For JPEG, `strip` removes EXIF APP1 segments, XMP APP1 segments, ICC APP2 segments, and COM comment segments before the scan data. The entropy-coded image stream is copied unchanged.

For WebP, `strip` removes `EXIF`, `XMP `, and `ICCP` RIFF chunks and clears the corresponding extended WebP feature flags. Image chunks are copied unchanged.

For TIFF, `strip` removes common metadata IFD entries such as description, camera/software strings, orientation, XMP, EXIF, GPS, IPTC, Photoshop, and ICC references from the first IFD. Referenced external metadata payload bytes are zeroed when their in-file byte range can be determined safely. Classic TIFF and BigTIFF first IFDs are supported.

## COPY BEHAVIOR

Without `--from`, `copy` preserves the complete recognized input image byte-for-byte.

With `--from`, `copy` performs selective PNG metadata transfer. Metadata chunks from METADATA_SOURCE replace metadata chunks in FILE and are inserted before image data in OUTPUT. Image data and non-metadata chunks from FILE are preserved.

## EDIT BEHAVIOR

For PNG, `edit --set-text KEY=VALUE` inserts or replaces an uncompressed `tEXt` chunk before image data. The new chunk is written with a fresh CRC. Existing critical chunks and image data are copied unchanged.

For PNG, `edit --set-itxt KEY=VALUE` inserts or replaces an `iTXt` chunk before image data. `--language TAG` stores a language tag in the new `iTXt` chunk. `--compressed` writes the text as a zlib stream using stored deflate blocks. The new chunk is written with a fresh CRC.

For PNG, `edit --remove-text KEY` removes matching `tEXt`, `iTXt`, and `zTXt` chunks while preserving image data and other chunks.

## LIMITATIONS

- `strip` is implemented for PNG, JPEG, WebP, classic TIFF, and BigTIFF first IFDs.
- Metadata editing currently supports PNG `tEXt` and PNG `iTXt` key/value entries. `zTXt` chunks can be listed by key and removed, but are not decompressed or edited yet.
- Selective metadata copying currently supports PNG metadata chunks only.
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
imgmeta strip -o clean.tiff scan.tiff
imgmeta copy -o preserved.jpg photo.jpg
imgmeta copy --from source.png -o with-metadata.png target.png
imgmeta list-text image.png
imgmeta edit --set-text comment=reviewed -o edited.png image.png
imgmeta edit --set-itxt caption=reviewed --language en -o edited.png image.png
imgmeta edit --set-itxt caption=reviewed --compressed -o edited.png image.png
imgmeta edit --remove-text comment -o clean-text.png edited.png
```

## SEE ALSO

imginfo, imgcheck, file
