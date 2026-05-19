# C2PA

## NAME

c2pa - add development C2PA manifests to images

## SYNOPSIS

```
c2pa add --dev-key -o OUTPUT [--claim-generator TEXT] [--action ACTION] FILE
```

## DESCRIPTION

`c2pa` creates C2PA/JUMBF metadata for supported image containers. The initial
implementation supports `add` for PNG and JPEG inputs that do not already carry
C2PA metadata.

The generated manifest contains a claim, a `c2pa.hash.data` assertion over the
original image bytes, a `c2pa.actions.v2` assertion, and a COSE_Sign1 ES256
signature. The signature is real and can be validated by `imgmeta`, `imginfo`,
and `imgcheck`, but it is made with a built-in development key and a minimal
embedded development certificate.

This is intended for local testing, fixtures, and development of the C2PA parser
and carrier-writing paths. It is not production provenance.

## OPTIONS

- `--dev-key` - required; use the built-in development P-256 key and certificate
- `-o`, `--output` - write the modified image to OUTPUT
- `--claim-generator TEXT` - set the claim generator string
- `--action ACTION` - set the simple action string stored in the claim
- `-h`, `--help` - show usage

## SUPPORTED FORMATS

- PNG: inserts a `caBX` chunk before `IEND`
- JPEG: inserts a C2PA APP11 JUMBF segment before scan data

## LIMITATIONS

- Existing C2PA metadata is rejected rather than updated or replaced.
- The signing key is built in and public; generated manifests are development
  fixtures, not trustworthy production credentials.
- The embedded certificate is minimal and only suitable for local parser and
  signature-verification tests.
- Trust-anchor policy, certificate path building, private-key import, and
  timestamp authority integration are not implemented yet.

## EXAMPLES

```
c2pa add --dev-key -o signed.png input.png
imgmeta show --c2pa-trust signed.png
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

