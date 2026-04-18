# NCC

## NAME

ncc - self-hosting C compiler for the project toolchain

## SYNOPSIS

```
ncc [OPTIONS] FILE
```

## DESCRIPTION

`ncc` is the repository compiler used to build and evolve the toolchain itself.
It supports hosted development today and is being extended toward the broader
freestanding system goals of the project.

## CURRENT CAPABILITIES

- Parsing and semantic analysis for the project C codebase
- Linux x86-64 code generation and ELF object output
- Support for the repository tools and self-hosting workflows
- Multiple backend targets under active development

## OPTIONS

- `-o FILE` — write output to FILE
- `-c` — compile to object file without linking
- `-S` — compile to assembly output

## LIMITATIONS

- Not a complete ISO C implementation; targets the project's own C subset.
- Only Linux x86-64 output is production-ready; other targets are in progress.

## EXAMPLES

```
ncc hello.c -o hello
ncc -S source.c -o source.s
ncc -c file.c -o file.o
```

## SEE ALSO

man, sh, project-layout
