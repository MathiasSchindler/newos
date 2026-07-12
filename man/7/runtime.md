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

- `memory.c` — freestanding memory primitives, size-class heap allocation,
  page-backed large allocations, checked array helpers, and arenas; see
  [memory](memory.md) for the allocator design
- `string.c` — string copying, comparison, parsing, and path joining helpers
- `parse.c` — numeric parsing used by command-line tools
- `io.c` — small buffered I/O wrappers over the platform layer
- `unicode.c` — UTF-8 decoding/encoding, validation, simple folding,
  whitespace/word classification, display width, and terminal text-segment scanning;
  common ASCII text is handled by fast paths before falling back to the Unicode
  range tables

### Shared utilities (`src/shared`)

- `tool_util.*` — common CLI parsing, error reporting, path, regex, and copy/remove helpers
- `simple_config.*` and `server_log.*` — small config parsing and escaped server logging used by daemon-style tools
- `tui.*` — terminal UI helpers used by interactive tools such as `editor` and `mail`
- `compression/` — reusable compression-adjacent primitives including CRC32,
  LZSS, zlib, and zstd support used by archive, metadata, and media tools
- `archive_util.*` — archive-format helpers and compatibility wrappers used by
  tar and compression tools
- `hash_util.*` and `crypto/` — MD5, SHA-2, public-key, ECDSA, X25519/Ed25519,
  ChaCha20-Poly1305, AES-GCM, X.509, and protocol-specific crypto helpers;
  RSA-compatible modular exponentiation uses Montgomery arithmetic for odd
  moduli and falls back to the generic reducer otherwise
- `tls/` — compact TLS 1.2/1.3 client-side machinery used through the platform
  TLS interface; X.509 verification caches the loaded trust bundle and parsed
  trust anchors within a process so tools with multiple HTTPS/TLS connections
  avoid repeated CA file reads, PEM decoding, and root-certificate parsing
- `image/` — metadata probing and structural validation for common image containers and C2PA-related tooling
- `xml.*`, `xml_stream.*`, and `xml_dtd.*` — reusable XML parsing, streaming, safety, and DTD support for the XML tool family
- `bignum.*` — fixed-capacity arbitrary-precision arithmetic used by math-oriented tools
- `math.*` — dependency-free binary64 elementary, trigonometric, hyperbolic, rounding, and power helpers; see [math](math.md) for the supported numerical profile

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
- There is no `FILE *`/stdio abstraction or locale database. Project concurrency is deliberately narrow and opt-in; see [threading](threading.md) for the intended task-pool, I/O-loop, and synchronization-substrate model.
- Formatting and parsing support cover the project's needs, not every libc edge case
- Terminal display width is compact rather than locale-complete: ANSI escapes,
  combining marks, wide CJK/emoji ranges, variation selectors, and emoji skin-tone
  modifiers are handled, but full grapheme-cluster shaping such as every ZWJ emoji
  family sequence remains approximate.

## SEE ALSO

man, project-layout, platform, memory, math, threading, shell, compiler
