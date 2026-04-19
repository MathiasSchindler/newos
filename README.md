# newos

newos is an experimental userland project for a Linux-ABI-compatible operating system.

In broad terms, this repository is a growing collection of command-line tools, shell support code, a self-hosting C compiler, shared runtime code, platform backends, and in-tree documentation designed to build on host systems such as macOS and Linux while also targeting a freestanding Linux environment.

The project has been written with the help of a finetuned version of GPT 5.4, with an emphasis on portability, small utilities, clear separation between tool logic and platform-specific code, and a freestanding-first design.

## Scope

The repository currently focuses on:

- a broad and growing Unix-style userland of command-line programs
- shell support and shared support code for strings, I/O, archives, and hashing
- the self-hosting C compiler ncc and its supporting infrastructure
- platform layers for hosted POSIX builds and freestanding Linux targets
- a small in-tree manual system, with pages stored under [man](man) and viewable through the project's own man tool

## Current status

The host-side build and smoke-test workflow are active and in regular use on macOS.

The userland has expanded substantially, the hosted Linux workflow is now active alongside macOS, and the compiler is already capable of handling a large portion of the repository, though the newest additions still expose some remaining self-hosting gaps that are being closed incrementally.

The repository also now includes a lightweight manual browser and a growing set of manual pages for tools, subsystems, and design notes.

## Testing

The repository now includes a structured smoke-test suite under [tests](tests).

- the entry point is [tests/run_smoke_tests.sh](tests/run_smoke_tests.sh)
- shared helpers live in [tests/lib](tests/lib)
- grouped suites live in [tests/suites](tests/suites)

You can run the suite with make test.

## Documentation

Manual pages live under [man](man), with user-facing commands typically documented in [man/1](man/1) and broader design notes in [man/7](man/7).

The repository also includes its own man program at [src/tools/man.c](src/tools/man.c), so the same tree can provide and browse its own documentation after a build.

## Benchmarks

A separate benchmark area now lives under [tests/benchmarks](tests/benchmarks) for host-side performance comparisons against the standard system tools.

You can run it with make benchmark.

## Contributing: shared CLI/options helper

Tools that need non-trivial option parsing use the lightweight `ToolOptState`
facility declared in [`src/shared/tool_util.h`](src/shared/tool_util.h) and
implemented in [`src/shared/tool_cli.c`](src/shared/tool_cli.c).

**Pattern** (`ssh`, `sed`, `grep`, `make` are migrated examples):

```c
ToolOptState s;
int r;
tool_opt_init(&s, argc, argv, "mytool", "[-x] [-f FILE] [args ...]");
while ((r = tool_opt_next(&s)) == TOOL_OPT_FLAG) {
    if (rt_strcmp(s.flag, "-f") == 0) {
        if (tool_opt_require_value(&s) != 0) return 1;
        path = s.value;
    } else if (rt_strcmp(s.flag, "-x") == 0) {
        opt_x = 1;
    } else {
        tool_write_error(s.prog, "unknown option: ", s.flag);
        tool_write_usage(s.prog, s.usage_suffix);
        return 1;
    }
}
if (r == TOOL_OPT_HELP) { /* print extended help */; return 0; }
/* s.argi is now the index of the first positional argument */
```

Key properties: `-h`/`--help` detection, `--` end-of-options, proper
error messages for missing option arguments, no heap allocation.


