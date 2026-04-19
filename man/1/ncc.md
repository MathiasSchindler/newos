# NCC

## NAME

ncc - self-hosting C compiler for the project toolchain

## SYNOPSIS

```
ncc [OPTIONS] FILE
```

## DESCRIPTION

`ncc` is the repository compiler used to build and evolve the toolchain itself.
It can preprocess, inspect compiler stages, emit assembly or objects, and on
supported host targets produce a runnable executable. The focus remains the
project's own codebase and self-hosting workflows rather than full GCC/Clang
compatibility.

## CURRENT CAPABILITIES

- preprocessing with `-E` and additional include or define options
- token, AST, and IR dumps for compiler debugging
- assembly output with `-S` and object output with `-c`
- default executable output on supported targets when linking succeeds
- target selection for `linux-x86_64`, `linux-aarch64`, and `macos-aarch64`

## OPTIONS

- `-E`, `--preprocess` — stop after preprocessing and write preprocessed output
- `-S`, `--emit-asm` — compile to assembly output
- `-c` — compile to object file without linking
- `-o FILE` — write output to FILE
- `-I DIR` — add an include search directory
- `-DNAME[=VALUE]` — define a preprocessor macro
- `--dump-tokens`, `--dump-ast`, `--dump-ir` — print intermediate compiler
  stages for inspection
- `--target TARGET` — choose a backend target
- `--help` — print command usage and supported targets
- `--version` — print the compiler stage/version string

## LIMITATIONS

- only one input source file is accepted per invocation
- not a complete ISO C implementation; the supported subset is aimed at the
  project's own code
- final executable linking currently relies on the host `clang` toolchain
- Linux x86-64 is the best-supported target today; Linux AArch64 output and
  linking still report "not implemented yet"

## EXAMPLES

```
ncc -E -I include -DDEBUG=1 hello.c
ncc hello.c -o hello
ncc --dump-ir source.c
ncc -S source.c -o source.s
ncc -c file.c -o file.o
```

## SEE ALSO

man, sh, project-layout
