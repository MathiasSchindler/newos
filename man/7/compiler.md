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
- parsing and semantic analysis for the bulk of the repository sources, including
  empty external declarations at translation-unit scope
- assembly generation for `linux-x86_64`, `linux-aarch64`, and
  `macos-aarch64`
- object emission for Linux/x86-64 ELF and macOS/AArch64 Mach-O
- self-hosted rebuilds of all 214 current tools as static no-libc Linux/x86-64
  executables, including `ncc` itself, using the in-tree linker for final links
- native macOS/AArch64 object and executable validation for focused compiler
  frontend and backend coverage

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

The current optimization work should stay in the target-neutral layers when
possible. A small IR cleanup pass already folds pure integer expressions and
same-value integer comparisons, simplifies neutral arithmetic identities and
safe short-circuit logical expressions, folds some side-effect-free same-arm
conditional expressions, and prunes unreachable instructions after constant
control-flow before any backend-specific assembly is emitted. Backend code
generation also applies small target-specific instruction selections, such as
shift-based scaling for power-of-two pointer offsets.

## SELF-HOSTING STAGES

The practical progression looks like this:

1. bootstrap with the host compiler
2. compile the project's own runtime and userland reliably
3. rebuild the static no-libc tool set with `ncc` and the in-tree linker in a
  separate self-host tree
4. remove the remaining bootstrap and external assembler dependencies

The current stage-3 check rebuilds the complete canonical tool list, runs a
no-libc smoke suite, and runs the native compiler regression suite through the
resulting `ncc`. It does not yet remove the initial host-compiler bootstrap,
Bash orchestration, or the external assembler used for `.S` sources.

## CONTRIBUTOR NOTES

- Front-end language work usually belongs in the preprocessor, lexer, parser, or
  semantic stages.
- ABI, calling convention, or register issues usually belong in
  `backend*.c` or `object_writer.c`.
- When debugging compiler changes, prefer `-E`, `-S`, or the `--dump-*`
  switches before assuming a runtime or linker bug.

## LIMITATIONS

- `ncc` targets the project's C subset, not full ISO C11 compatibility
- floating-point types, literals, arithmetic, conversions, and ABI classification are not implemented; declarations using `double` currently lower as 8-byte integers and cannot execute the shared math API correctly
- `linux-aarch64` assembly generation exists, but direct object emission is not finished yet
- Optimization, debug-info emission, and linker sophistication are still modest

## SEE ALSO

man, ncc, project-layout, build, runtime, math
