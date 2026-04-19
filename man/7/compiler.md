# COMPILER

## NAME

compiler - the `ncc` compiler and its internal architecture

## DESCRIPTION

The compiler lives in `src/compiler` and is exposed through the `ncc` tool. It
is best thought of as a practical project compiler: strong enough to build and
debug the repository's C subset, but still narrower than a full system C
compiler.

## CURRENT CAPABILITIES

- preprocessing with `-E`, `-I`, and simple `-DNAME=VALUE` defines
- token, AST, and IR dumps for debugging (`--dump-tokens`, `--dump-ast`,
  `--dump-ir`)
- parsing and semantic analysis for the bulk of the repository sources
- assembly generation for `linux-x86_64`, `linux-aarch64`, and
  `macos-aarch64`
- object emission for Linux/x86-64 ELF and macOS/AArch64 Mach-O
- partial self-hosting across most of its own source tree

## SUCCESS CRITERIA

The compiler is doing the right job when it can:

1. compile the shared runtime and support code used across the repository
2. compile the userland tools under `src/tools/`
3. rebuild itself in staged form with steadily less outside toolchain help
4. stay understandable enough that contributors can still debug and extend it

## WORKFLOW

Compilation proceeds in these layers:

1. `source.c` — file loading and location tracking
2. `preprocessor.c` — macro expansion and conditional compilation
3. `lexer.c` — tokenization
4. `parser*.c` — AST construction
5. `semantic.c` — type checking and semantic validation
6. `ir.c` — lowering to the internal linear IR
7. `backend*.c` — target-specific code generation
8. `object_writer.c` — object-file serialization
9. `driver.c` — command-line handling and orchestration

## SELF-HOSTING STAGES

The practical progression looks like this:

1. bootstrap with the host compiler
2. compile the project's own runtime and userland reliably
3. rebuild `ncc` with itself in repeatable stages
4. reduce dependence on the external host linker/toolchain where practical

## CONTRIBUTOR NOTES

- Front-end language work usually belongs in the preprocessor, lexer, parser, or
  semantic stages.
- ABI, calling convention, or register issues usually belong in
  `backend*.c` or `object_writer.c`.
- When debugging compiler changes, prefer `-E`, `-S`, or the `--dump-*`
  switches before assuming a runtime or linker bug.

## LIMITATIONS

- `ncc` targets the project's C subset, not full ISO C11 compatibility
- `linux-aarch64` assembly generation exists, but direct object emission is not
  finished yet
- Optimization, debug-info emission, and linker sophistication are still modest

## SEE ALSO

man, ncc, project-layout, build, runtime
