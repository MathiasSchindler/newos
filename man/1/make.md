# MAKE

## NAME

make - build targets described in a Makefile

## SYNOPSIS

make [-n] [-s] [-B] [-f makefile] [-C dir] [-j [jobs]]
     [VAR=value ...] [target ...]

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
- dry-run mode
- global silent mode (`-s`)
- force rebuild mode (`-B`)
- changing into a build directory with `-C`
- GNU-style `-j` parsing with `MAKEFLAGS` propagation for recursive compatibility
- styled diagnostics and usage output with shared terminal color support

## OPTIONS

- `-n` — print commands that would be executed without running them
- `-s`, `--silent` — suppress recipe echoing
- `-B`, `--always-make` — force targets with recipes to rebuild
- `-f makefile` — read `makefile` instead of the default `Makefile`,
  `makefile`, or `GNUmakefile`
- `-C dir` — change to `dir` before reading the makefile and building targets
- `-j [jobs]` — accept a GNU-compatible job count flag and expose it through `MAKEFLAGS`
- `VAR=value` — override or define a variable on the command line
- `--color[=WHEN]` — control styled diagnostic output using `auto`, `always`, or `never`
- `--help` — print the command usage summary

## LIMITATIONS

- parallel recipe execution is still not implemented; `-j` is accepted for compatibility and recursive propagation
- some advanced GNU make features (e.g. `$(eval ...)`, load directives) are not implemented

Color output follows the shared project behavior documented in `output-style`.

## NOTES

- Command-line variable assignments override ordinary Makefile assignments for
  that invocation.
- As in traditional `make`, each recipe line normally runs through the shell on
  its own; combine commands with `&&` when one line depends on the previous one.

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
