# MAKE

## NAME

make - build targets described in a Makefile

## SYNOPSIS

make [-n] [-f makefile] [VAR=value ...] [target ...]

## DESCRIPTION

make reads a Makefile (or the file named by `-f`) and builds the specified targets by executing their recipe commands. Dependencies are tracked and a target is only rebuilt when it is out of date relative to its prerequisites.

## CURRENT CAPABILITIES

- parsing and executing Makefile rules and recipes
- variable assignment and expansion (`$(VAR)`)
- automatic variables: `$@` (target), `$<` (first prerequisite), `$^` (all prerequisites), `$*` (stem)
- pattern rules (`%.o: %.c`)
- phony targets (`.PHONY`)
- `include` directives
- conditional blocks (`ifeq`, `ifneq`, `ifdef`, `ifndef`)
- built-in functions: `$(wildcard ...)`, `$(patsubst ...)`, `$(subst ...)`, `$(strip ...)`, `$(filter ...)`, `$(filter-out ...)`, `$(sort ...)`, `$(addprefix ...)`, `$(addsuffix ...)`, `$(dir ...)`, `$(notdir ...)`, `$(basename ...)`, `$(suffix ...)`, `$(firstword ...)`, `$(lastword ...)`, `$(words ...)`, `$(word ...)`, `$(foreach ...)`, `$(if ...)`, `$(origin ...)`, `$(shell ...)`
- dry-run mode

## OPTIONS

- `-n` — print commands that would be executed without running them
- `-f makefile` — read makefile as the Makefile instead of the default `Makefile` or `makefile`
- `VAR=value` — override or define a variable on the command line

## LIMITATIONS

- parallel execution (`-j`) is not supported
- recursive make (`$(MAKE)`) invokes the same binary but does not pass job-count flags
- some advanced GNU make features (e.g. `$(eval ...)`, load directives) are not implemented

## EXAMPLES

- `make` — build the default target
- `make -n` — show what would be built
- `make -f build.mk install` — use an alternate Makefile and build the install target
- `make CC=clang all` — override the CC variable

## SEE ALSO

sh, ncc
