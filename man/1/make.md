# MAKE

## NAME

make - build targets described in a Makefile

## SYNOPSIS

make [-n] [-f makefile] [-C dir] [VAR=value ...] [target ...]

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
- changing into a build directory with `-C`

## OPTIONS

- `-n` — print commands that would be executed without running them
- `-f makefile` — read makefile as the Makefile instead of the default `Makefile` or `makefile`
- `-C dir` — change to `dir` before reading the makefile and building targets
- `VAR=value` — override or define a variable on the command line

## LIMITATIONS

- parallel execution (`-j`) is not supported
- recursive make (`$(MAKE)`) invokes the same binary but does not pass job-count flags
- some advanced GNU make features (e.g. `$(eval ...)`, load directives) are not implemented

## NOTES

- Command-line variable assignments override ordinary Makefile assignments for
  that invocation.
- As in traditional `make`, each recipe line normally runs through the shell on
  its own; combine commands with `&&` when one line depends on the previous one.

## EXAMPLES

- `make` — build the default target
- `make -n` — show what would be built
- `make -C src test` — change into `src` and build the `test` target
- `make -f build.mk install` — use an alternate Makefile and build the install target
- `make CC=clang all` — override the CC variable

## SEE ALSO

sh, ncc
