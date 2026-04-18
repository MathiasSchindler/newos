# RUNTIME

## NAME

runtime - the shared runtime library and utility modules used across tools

## DESCRIPTION

The runtime component provides the low-level building blocks that all tools rely on. It is divided into a core runtime layer (src/shared/runtime) and a set of shared utility modules (src/shared). These sources are compiled into every tool and replace the C standard library functions that would otherwise require libc linkage, supporting both the hosted and the freestanding build targets.

## CURRENT CAPABILITIES

### Core runtime (src/shared/runtime)

- memory.c — heap allocation, reallocation, and freeing built on top of the platform memory interface
- string.c — string operations: copy, compare, length, search, conversion to and from integers
- parse.c — number parsing helpers used by tools that process numeric input
- io.c — buffered read and write wrappers over the platform I/O interface

### Shared utilities (src/shared)

- tool_util.c / tool_util.h — common argument parsing and error reporting helpers used by most tools
- archive_util.c / archive_util.h — tar-format archive reading and writing
- hash_util.c / hash_util.h — MD5, SHA-256, and SHA-512 implementations used by the checksum tools

### Header interfaces

- src/shared/runtime.h — public runtime API included by all tools
- src/shared/platform.h — thin abstraction over platform-specific I/O and memory primitives
- src/shared/shell_shared.h — shell-specific constants and types (included only by the shell subsystem)

## LIMITATIONS

- The runtime intentionally provides only the subset of functionality needed by the project tools; it is not a general-purpose libc replacement
- Locale and wide-character support are absent
- Threading primitives are not provided

## SEE ALSO

man, project-layout, platform, shell, compiler
