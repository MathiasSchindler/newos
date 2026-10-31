# PROJECT-LAYOUT

## NAME

project-layout - overview of the repository structure and layering rules

## DESCRIPTION

The tree is organized by responsibility: user-facing tools, reusable shared
code, compiler internals, and platform-specific code are kept separate so that
hosted and freestanding builds can share as much logic as possible.

The project now also follows a stronger ownership rule for tools: each command
keeps one public entry file in `src/tools/`, and if it grows additional private
implementation files they live in a same-named subdirectory owned by that tool.

## STRUCTURE

- `src/tools/` — command entry points and tool-specific logic; each tool keeps
  one public entry file here, and larger tools may also own a private
  subdirectory such as `src/tools/awk/`
- `src/shared/` — reusable runtime helpers and cross-tool utilities; this is
  the home for genuinely shared code, not one-off private helper files
- `src/shared/math/` — dependency-free binary64 helpers and fixed-capacity
  arbitrary-precision integer arithmetic
- `src/shared/xml/` — XML parsing, streaming, DTD support, and XML tool helpers;
  XML-specific tool declarations live in `xml/tool_xml.h`, not `tool_util.h`
- `src/shared/ssh/` — reusable SSH transport/client support shared by `ssh`,
  `sshd`, and Git-over-SSH remote operations
- `src/compiler/` — the `ncc` frontend, IR, backends, and object writers
- `src/platform/posix/` — hosted development and test implementation
- `src/platform/linux/` — freestanding raw-syscall implementation
- `src/platform/macos/` — native macOS arm64 project-linked backend
- `src/platform/windows/` — native Windows freestanding PE backend and startup
  support
- `src/arch/aarch64/linux/` — startup and syscall ABI glue for the freestanding
  AArch64 target
- `src/arch/x86_64/linux/` — startup and syscall ABI glue for the freestanding
  x86-64 target
- `src/arch/aarch64/macos/` — Darwin syscall ABI helpers and experimental
  startup code for macOS arm64 work
- `tests/` — smoke suites, helpers, benchmarks, Windows bring-up scripts, and
  run-time logs under `tests/tmp/`
- `scripts/` — build/reporting helpers and Makefile-backed convenience
  launchers kept out of the repository root
- `man/` — repository-local manuals; `man/1` and `man/7` are the active home for
  current documentation and contributor-facing project notes
- `services/` — hosted daemon instance data such as document roots, configs, and logs; implementation still belongs under `src/tools/`
- `build/` — generated binaries and other build output

## LAYERING RULES

- Most tools should depend on `src/shared/` and `platform.h`, not on direct OS
  headers or compiler internals.
- Each tool should present one obvious public entry file in `src/tools/`; if it
  needs extra internal modules, place them under `src/tools/<tool>/` rather
  than flattening them into the top-level tool directory.
- `src/shared/` is reserved for code that is reused by more than one tool or is
  intentionally maintained as a reusable subsystem.
- Shell-specific code stays in `src/tools/sh/` and is compiled only into `sh`.
- `src/compiler/` is largely private to `ncc`; other tools should not grow hard
  dependencies on it.
- OS-specific behavior belongs in `src/platform/*` or `src/arch/*`, not in the
  generic tool code.
- Legacy planning notes may still exist elsewhere in the tree, but the manual
  pages under `man/` should be treated as the current source of truth.

## SHARED TOOL HELPERS

The root-level `tool_*.c` files share concrete implementations, not interchangeable
backends. Their declarations remain in `tool_util.h`, except for XML helpers:

| Implementation | Contents |
| --- | --- |
| `tool_io.c` | Descriptor-based streaming, record readers, buffered output, and direct output helpers |
| `tool_file.c` | Input opening/closing and whole-input reads / whole-file writes |
| `tool_fs.c` | Copying, removal, tree walks, parent-directory creation, replacement policy, and symbolic permissions |
| `tool_path.c` | Path names, joining, resolution, and filesystem identity checks |
| `tool_cli.c` | Option iteration and attached values, usage/errors, interactive confirmation, and file-header policy |
| `tool_json.c` | Shared structured-output mode and event writing |
| `tool_buffer.c` | Byte buffers, arrays, fixed records, and bounded buffer appends |
| `tool_binary.c` | Byte-order loads/stores, byte comparison, and hex/base64 digit conversion |
| `tool_parse.c` | Numeric, address, size, and escaped-string parsing |
| `tool_text.c` | Text comparison/matching, ASCII folding shared by process/regex matching, ASCII/UTF-8 predicates, word boundaries, display width, and tabstops |
| `tool_regex.c` | Regular-expression compilation and matching |
| `tool_format.c` | Integer-base, size, and block-size formatting; decimal digit counts |
| `tool_time.c` | Timezones, durations, compact uptime, and civil-date arithmetic |
| `tool_terminal.c` | Color/style policy, terminal-mode restoration, and pager sizing |
| `tool_http.c` | HTTP status/header delimiters and direct HTTP/TLS connection operations |
| `tool_object.c` | Object-symbol classification, display names, and Mach-O ARM64 relocation names |
| `tool_process.c` | Process matching, signals, PID filters, program-path validation, and worker-count selection |

Crypto filenames describe their implemented families: `crypto/aes.{c,h}` and
`crypto/aes_gcm.{c,h}` support AES-128 and AES-256; `crypto/hmac_sha2.{c,h}`
supports HMAC-SHA256 and HMAC-SHA384. Algorithm-specific public function names
remain explicit. `usb_descriptor.c` pairs with `usb_descriptor.h` and contains
descriptor parsing, not an OS USB transport backend. Renderer headers live
directly beside their implementations in `fontrender/`.

## LIMITATIONS

- The Linux freestanding path currently targets AArch64 and x86-64
- macOS is the main developer workstation platform, and `make freestanding` is
  the preferred local no-libSystem project-linked arm64 build; it is not a fully
  separate Darwin userland target in the Linux sense
- Windows contributor work currently assumes MSYS2 for hosted POSIX builds; native Windows freestanding PE support is present but still growing
- Manual pages are kept in-tree and may lag very recent behavior changes until they are refreshed alongside the code

## SEE ALSO

man, shell, compiler, runtime, platform, foundry, macos, testing, build, userland
