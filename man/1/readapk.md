# READAPK

## NAME

**readapk** - inspect Android APK container structure

## SYNOPSIS

```
readapk [-a] [-s] [-l] [--verify] [--manifest] [--resources] [--dex]
  [--native] [--signatures] [--dates] [--capabilities]
  [--resources-detail] [--files-detail] [--code-detail] [--security]
  [--extract-manifest DIR] [--extract-dex DIR] [--extract-native DIR]
  [--extract-signatures DIR] [--extract-resource ID] [--json] FILE ...
```

## DESCRIPTION

`readapk` inspects Android APK files. APKs are ZIP-family containers, like JAR
files, with Android-specific entries and optional APK Signing Blocks layered
around the ZIP central directory.

The tool reads ZIP metadata directly and does not need Java, Android SDK tools,
or libc archive libraries. It reports APK-oriented structure such as
`AndroidManifest.xml`, `resources.arsc`, DEX files, native libraries, `res/`,
`assets/`, `META-INF/` v1 signature files, Zip64 markers, and APK v2/v3 Signing
Block presence. The default summary also gives a quick overview of payload size,
largest entry, ZIP timestamp range, resource mix, and native ABI coverage when
present. Optional deeper modes validate ZIP structure, decode selected Android
binary XML manifest attributes, summarize resource tables, inspect DEX headers,
summarize embedded ELF shared libraries, classify capabilities, and report APK
signing layout.

Options:

- **-s**, **--summary** - print the APK/ZIP summary
- **-l**, **--list** - list ZIP central-directory entries
- **-a**, **--all** - print both summary and entry list
- **--verify** - validate ZIP/APK structure, local headers, safe entry names,
  supported compression methods, and CRCs for readable stored/deflated entries
- **--manifest** - decode high-value `AndroidManifest.xml` fields from Android
  binary XML, including package/version, SDK levels, permissions, components,
  and selected application flags; string/path resource references are resolved
  through `resources.arsc` when possible
- **--resources** - summarize `resources.arsc` table chunks, packages, string
  pools, type specs, and type configurations
- **--dex** - summarize DEX headers, index counts, and the first class
  descriptors from each DEX file
- **--native** - inspect `lib/*/*.so` entries as ELF shared libraries and report
  ABI, ELF class, machine, type, entry address, program headers, and sections
- **--signatures** - report v1 signature files, APK Signing Block placement,
  v2/v3/v3.1/source-stamp ID presence, v2/v3 signer/signature/certificate
  counts, and cryptographic verification status
- **--dates** - list ZIP entry timestamps, which are useful for build/repack
  comparisons but may be synthetic build-system values
- **--capabilities** - summarize manifest permissions, permission groups,
  hardware/software features, app metadata, component exports, intent filters,
  package queries, and libraries
- **--resources-detail** - list resource and asset entries and include the
  `resources.arsc` table summary
- **--files-detail** - classify every entry by APK role and show compressed and
  uncompressed sizes plus compression method
- **--code-detail** - inspect DEX files and native libraries in one code-focused
  mode
- **--security** - summarize sensitive permissions, exported components,
  security-relevant application flags, ZIP validation, and APK signing layout
- **--extract-manifest DIR** - write decoded manifest text to
  `DIR/AndroidManifest.txt`, resolving simple resource references when possible
- **--extract-dex DIR** - extract only DEX entries to `DIR`, preserving APK
  entry paths such as `classes.dex` or `classes2.dex`
- **--extract-native DIR** - extract only native libraries to `DIR`, preserving
  `lib/<abi>/*.so` paths
- **--extract-signatures DIR** - extract `META-INF/*` entries and the raw APK
  Signing Block, when present, to `DIR`
- **--extract-resource ID** - resolve a simple resource ID such as
  `0x7f1301da` through `resources.arsc` and print the value
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
readapk --verify app.apk
readapk --manifest app.apk
readapk --capabilities app.apk
readapk --security app.apk
readapk --dates --files-detail app.apk
readapk --resources-detail app.apk
readapk --code-detail app.apk
readapk --dex --native app.apk
readapk --signatures app.apk
readapk --extract-manifest out app.apk
readapk --extract-dex dex-out --extract-native native-out app.apk
readapk --extract-signatures sig-out app.apk
readapk --extract-resource 0x7f1301da app.apk
readapk --json -a app.apk
```

## LIMITATIONS

- Android binary XML and resource table support is intentionally summary-level;
  simple string and path references can be resolved, but `readapk` is not a
  complete `aapt` replacement.
- Capability and security summaries are manifest-oriented. They surface useful
  static signals, but they do not prove runtime behavior.
- ZIP timestamps are reported as stored in the archive. Many Android build
  systems intentionally normalize these values.
- Extraction modes are APK-aware filters. They are not a general-purpose ZIP
  extractor, and they reject unsafe entry paths such as absolute paths,
  backslashes, `.`/`..` components, symlinked output paths, and overlong paths.
- DEX support reports headers, counts, and selected class descriptors; it does
  not disassemble bytecode.
- Native library support reports ELF headers; detailed dynamic-symbol and
  hardening analysis should be done with `readelf` on an extracted library.
- Signature files and APK Signing Blocks are located and classified, and v2/v3
  signer/signature/certificate counts are parsed. Full certificate-chain,
  signer digest, and cryptographic APK signature validation are not implemented
  yet.
- Multi-disk ZIP archives are detected, but ordinary APKs are expected to be
  single-file archives.

## JSON Output

With `--json`, `readapk` emits JSON Lines using the common envelope documented
in `json-output`. Implemented events are `apk_entry` and `apk_summary`.

Example event:

```json
{"schema":"newos.tool.v1","tool":"readapk","stream":"stdout","event":"apk_summary","seq":1,"data":{"file":"app.apk","probable_apk":true,"entries":6,"dex_files":1}}
```
