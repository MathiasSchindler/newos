# RUNTIME

## NAME

runtime - the shared support code compiled into the project tools

## DESCRIPTION

The runtime is the portability layer that most tools sit on top of. It is not a
separately versioned library or a full libc replacement; it is a compact set of
helpers compiled directly into the tools so the same sources work in both the
hosted and freestanding builds.

## CURRENT CAPABILITIES

### Core runtime (`src/shared/runtime`)

- `memory.c` — allocation helpers built on the platform memory interface
- `string.c` — string copying, comparison, parsing, and path joining helpers
- `parse.c` — numeric parsing used by command-line tools
- `io.c` — small buffered I/O wrappers over the platform layer
- `unicode.c` — UTF-8 decoding/encoding, validation, simple folding,
  whitespace/word classification, display width, and terminal text-segment scanning

### Shared utilities (`src/shared`)

- `tool_util.*` — common CLI parsing, error reporting, path, regex, and copy/remove helpers
- `simple_config.*` and `server_log.*` — small config parsing and escaped server logging used by daemon-style tools
- `tui.*` — terminal UI helpers used by interactive tools such as `editor` and `mail`
- `compression/` — reusable compression-adjacent primitives including CRC32,
  LZSS, zlib, and zstd support used by archive, metadata, and media tools
- `archive_util.*` — archive-format helpers and compatibility wrappers used by
  tar and compression tools
- `hash_util.*` and `crypto/` — MD5, SHA-2, public-key, ECDSA, X25519/Ed25519,
  ChaCha20-Poly1305, AES-GCM, X.509, and protocol-specific crypto helpers
- `tls/` — compact TLS 1.2/1.3 client-side machinery used through the platform TLS interface
- `image/` — metadata probing and structural validation for common image containers and C2PA-related tooling
- `xml.*`, `xml_stream.*`, and `xml_dtd.*` — reusable XML parsing, streaming, safety, and DTD support for the XML tool family
- `bignum.*` — fixed-capacity arbitrary-precision arithmetic used by math-oriented tools

## CONTRIBUTOR BOUNDARIES

- Put genuinely reusable, libc-independent helpers here.
- If logic is only used by one command, keep it in that tool instead of growing
  the runtime unnecessarily.
- Tool-private helper modules should live under `src/tools/<tool>/`, with the
  tool's public entry point kept in `src/tools/<tool>.c`.
- Code in this layer should go through `platform.h` for OS interaction rather
  than calling libc or POSIX APIs directly.

## LIMITATIONS

- This is a focused internal support layer, not a general-purpose standard library
- There is no `FILE *`/stdio abstraction, locale database, or broad userland threading API; runtime threading support is limited to specific shared subsystems such as allocator locking
- Formatting and parsing support cover the project's needs, not every libc edge case

## SEE ALSO

man, project-layout, platform, shell, compiler
