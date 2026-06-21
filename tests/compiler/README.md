# Compiler tests and benchmarks

`tests/compiler/` contains focused checks for compiler behavior that are useful
outside the normal tool-level Phase 1 suite.

## Synthetic benchmarks

Run the compiler code-quality benchmark suite from the repository root with:

```sh
make compiler-benchmark
```

or directly with:

```sh
./tests/compiler/run_compiler_benchmarks.sh
```

The runner builds every `tests/compiler/benchmarks/*.c` case with the host C
compiler and with `ncc`, then reports executable size, generated assembly line
count, average runtime, and the ncc/gcc runtime ratio. Runtime measurement uses
the in-tree `time` tool when available. Results are informational and intended
for trend tracking while improving ncc code generation.
