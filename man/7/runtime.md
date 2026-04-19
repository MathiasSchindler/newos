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

### Shared utilities (`src/shared`)

- `tool_util.*` — common CLI parsing, error reporting, path, regex, and copy/remove helpers
- `archive_util.*` — tar archive support
- `hash_util.*` — MD5, SHA-256, and SHA-512 implementations

## CONTRIBUTOR BOUNDARIES

- Put genuinely reusable, libc-independent helpers here.
- If logic is only used by one command, keep it in that tool instead of growing
  the runtime unnecessarily.
- Code in this layer should go through `platform.h` for OS interaction rather
  than calling libc or POSIX APIs directly.

## LIMITATIONS

- This is a focused internal support layer, not a general-purpose standard
  library
- There is no `FILE *`/stdio abstraction, locale support, wide-character
  support, or threading API
- Formatting and parsing support cover the project's needs, not every libc edge
  case

## SEE ALSO

man, project-layout, platform, shell, compiler
