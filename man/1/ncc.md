# NCC

## NAME

ncc - self-hosting C compiler for the project toolchain

## SYNOPSIS

ncc [OPTIONS] FILE

## DESCRIPTION

ncc is the repository compiler used to build and evolve the toolchain itself. It supports hosted development today and is being extended toward the broader freestanding system goals of the project.

## CURRENT CAPABILITIES

- parsing and semantic analysis for the project codebase
- Linux x86-64 code generation and ELF object output
- support for the repository tools and self-hosting workflows
- multiple backend targets under active development

## EXAMPLES

- ncc hello.c -o hello
- ncc -S source.c -o source.s
- ncc -c file.c -o file.o

## SEE ALSO

man, sh, project-layout
