# READAPK

## NAME

**readapk** - inspect Android APK container structure

## SYNOPSIS

```
readapk [-a] [-s] [-l] [--json] FILE ...
```

## DESCRIPTION

`readapk` inspects Android APK files. APKs are ZIP-family containers, like JAR
files, with Android-specific entries and optional APK Signing Blocks layered
around the ZIP central directory.

The tool reads ZIP metadata directly and does not need Java, Android SDK tools,
or libc archive libraries. It reports APK-oriented structure such as
`AndroidManifest.xml`, `resources.arsc`, DEX files, native libraries, `res/`,
`assets/`, `META-INF/` v1 signature files, Zip64 markers, and APK v2/v3 Signing
Block presence.

Options:

- **-s**, **--summary** - print the APK/ZIP summary
- **-l**, **--list** - list ZIP central-directory entries
- **-a**, **--all** - print both summary and entry list
- **--json** - emit JSON Lines using the common tool envelope

If no mode is selected, `readapk` prints the summary.

The entry-list flags column uses four characters:

- **E** - encrypted ZIP entry
- **D** - entry uses a data descriptor
- **U** - UTF-8 name flag is set
- **Z** - entry uses Zip64 extended metadata

## EXAMPLES

```
readapk app.apk
readapk -l app.apk
readapk -a app.apk
readapk --json -a app.apk
```

## LIMITATIONS

- `readapk` reads ZIP central-directory metadata and APK Signing Block IDs, but
  it does not decompress file contents.
- `AndroidManifest.xml` is detected as an APK entry, but binary Android XML is
  not decoded yet.
- Signature files and APK Signing Blocks are located and classified; signature
  certificate chains and digest verification are not implemented.
- Multi-disk ZIP archives are detected, but ordinary APKs are expected to be
  single-file archives.

## JSON Output

With `--json`, `readapk` emits JSON Lines using the common envelope documented
in `json-output`. Implemented events are `apk_entry` and `apk_summary`.

Example event:

```json
{"schema":"newos.tool.v1","tool":"readapk","stream":"stdout","event":"apk_summary","seq":1,"data":{"file":"app.apk","probable_apk":true,"entries":6,"dex_files":1}}
```
