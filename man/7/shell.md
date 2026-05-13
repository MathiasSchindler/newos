# SHELL

## NAME

shell - the `sh` command and its internal shell subsystem

## DESCRIPTION

The shell component consists of the `sh` tool plus a private subsystem in
`src/tools/sh/`. It provides an interactive command shell and script runner for
the repository userland, but it is still intentionally smaller than a full
production shell such as `bash`.

## CURRENT CAPABILITIES

- pipelines, redirections, semicolon-separated commands, and `&&`/`||`
- background jobs with `jobs`, `fg`, `bg`, and `wait`
- variable assignment and expansion, wildcard expansion, and command
  substitution with `$()`
- `if`, `while`, `for`, `case`, and shell-function definitions
- here-documents and aliases
- interactive history plus simple line editing and completion when run on a TTY

## INTERNAL LAYOUT

The public entry point remains `src/tools/sh.c`. Private shell implementation
files live under `src/tools/sh/`:

- `shell_parser.c` — tokenization and parse-tree construction
- `shell_execution.c` — process launching, pipelines, and job handling
- `shell_builtins.c` — built-in commands and shell state updates
- `shell_interactive.c` — line editing, prompt loop, and command history
- `shell_shared.h` — shared line-size constants and data structures

## CURRENT BOUNDARIES

- The subsystem is private to `sh`; other tools should not depend on it
- Interactive features only apply when the shell is attached to a terminal
- Pipelines, command arguments, jobs, history, aliases, and functions use
  growable storage rather than small fixed entry counts; allocation failures
  are reported as shell errors or leave optional state such as history unchanged
- Input is still processed in fixed-size logical lines, so individual commands,
  here-doc markers, function bodies, alias values, and stored job text must fit
  within the shell line buffer
- No process substitution, and shell-language compatibility is still narrower
  than `bash` or `dash`

## SEE ALSO

man, sh, project-layout, runtime
