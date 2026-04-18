# COMPILER

## NAME

compiler - the ncc self-hosting C compiler and its internal architecture

## DESCRIPTION

The compiler component lives in src/compiler and is exposed to users through the ncc tool (src/tools/ncc.c). It is a single-pass C compiler written in C11, capable of compiling the bulk of the repository itself. The design targets Linux x86-64 code generation and ELF object output while keeping the source portable enough to cross-compile via the freestanding workflow.

## CURRENT CAPABILITIES

- C11 preprocessing including macro expansion and conditional compilation
- full lexing and parsing of the project source base
- semantic analysis and type checking for the supported language subset
- IR generation from the parsed AST
- Linux x86-64 native code generation
- ELF object file output compatible with the system linker
- self-hosting: ncc can compile most of its own source files

## WORKFLOW

Compilation goes through the following stages in order:

1. source.c / source.h — source file reading and location tracking
2. preprocessor.c — token-level preprocessing and macro expansion
3. lexer.c / lexer.h — tokenisation of preprocessed text
4. parser.c (+ parser_types.c, parser_expressions.c, parser_declarations.c, parser_statements.c) — AST construction
5. semantic.c / semantic.h — type resolution and semantic checks
6. ir.c / ir.h — lowering to a simple linear IR
7. backend.c (+ backend_expressions.c, backend_codegen.c) — IR to x86-64 assembly or object code
8. object_writer.c / object_writer.h — ELF object serialisation
9. driver.c — top-level orchestration and option handling

## LIMITATIONS

- Only Linux x86-64 output is fully implemented; other targets are under development
- A subset of C11 features is not yet handled (complex numbers, VLAs in all contexts, some edge cases in designated initializers)
- Self-hosting is not yet complete for the newest tool additions
- No link-time optimisation or debug information output

## SEE ALSO

man, ncc, project-layout, build, runtime
