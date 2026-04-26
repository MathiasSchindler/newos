# MAKE

## NAME

make - build targets described in a Makefile

## SYNOPSIS

```
make [-n] [-s] [-B] [-f makefile] [-C dir] [-j [jobs]]
     [VAR=value ...] [target ...]
```

## DESCRIPTION

make reads a build file and executes the recipes needed to update the requested
targets. If `-f` is not used it looks for `Makefile`, `makefile`, or
`GNUmakefile` in that order.

## CURRENT CAPABILITIES

- build the default target (the first rule) or explicitly named targets
- parsing and executing Makefile rules and recipes
- variable assignment and expansion (`$(VAR)`)
- automatic variables: `$@` (target), `$<` (first prerequisite), `$^` (all prerequisites), `$*` (stem)
- pattern rules (`%.o: %.c`)
- phony targets (`.PHONY`)
- `include` directives
- conditional blocks (`ifeq`, `ifneq`, `ifdef`, `ifndef`)
- built-in functions: `$(wildcard ...)`, `$(patsubst ...)`, `$(subst ...)`, `$(strip ...)`, `$(filter ...)`, `$(filter-out ...)`, `$(sort ...)`, `$(addprefix ...)`, `$(addsuffix ...)`, `$(dir ...)`, `$(notdir ...)`, `$(basename ...)`, `$(suffix ...)`, `$(firstword ...)`, `$(lastword ...)`, `$(words ...)`, `$(word ...)`, `$(foreach ...)`, `$(if ...)`, `$(origin ...)`, `$(shell ...)`
- imported environment variables plus `export VAR` / `export VAR = value`
- `.ONESHELL` multi-line recipe execution and `SHELL` overrides
- GNU-compatible `+` recipe prefixes that still run under `-n`
- dry-run mode
- global silent mode (`-s`)
- force rebuild mode (`-B`)
- changing into a build directory with `-C`
- limited parallel builds for independent goals and prerequisite batches with `-j`
- styled diagnostics and usage output with shared terminal color support

## OPTIONS

- `-n` — print commands that would be executed without running them
- `-s`, `--silent` — suppress recipe echoing
- `-B`, `--always-make` — force targets with recipes to rebuild
- `-f makefile` — read `makefile` instead of the default `Makefile`,
  `makefile`, or `GNUmakefile`
- `-C dir` — change to `dir` before reading the makefile and building targets
- `-j [jobs]` — run up to `jobs` independent build goals/prerequisite batches concurrently; bare `-j` selects a small automatic default
- `VAR=value` — override or define a variable on the command line
- `--color[=WHEN]` — control styled diagnostic output using `auto`, `always`, or `never`
- `--help` — print the command usage summary

## LIMITATIONS

- parallel scheduling is intentionally smaller than full GNU `make`: it batches independent goals and prerequisite sub-builds, but does not implement the full jobserver protocol
- some advanced GNU make features (e.g. `$(eval ...)`, load directives) are not implemented

Color output follows the shared project behavior documented in `output-style`.

## NOTES

- Command-line variable assignments override ordinary Makefile assignments for
  that invocation.
- Environment variables are visible to the Makefile and can be forwarded to
  recipes with `export`.
- As in traditional `make`, each recipe line normally runs through the shell on
  its own; use `.ONESHELL:` when later lines depend on earlier shell state.
- Lines prefixed with `+` still execute in dry-run mode for recursive or
  side-effectful compatibility recipes.

## EXAMPLES

- `make` — build the default target
- `make -n` — show what would be built
- `make -s test` — build quietly
- `make -B all` — force a rebuild of `all`
- `make -C src test` — change into `src` and build the `test` target
- `make -f build.mk install` — use an alternate Makefile and build the install target
- `make CC=clang all` — override the CC variable

## SEE ALSO

sh, ncc, output-style
