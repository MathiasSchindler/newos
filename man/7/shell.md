# SHELL

## NAME

shell - the `sh` command and its internal shell subsystem

## DESCRIPTION

The shell component consists of the `sh` tool plus a private subsystem in
`src/shared/shell_*`. It provides an interactive command shell and script
runner for the repository userland, but it is still intentionally smaller than a
full production shell such as `bash`.

## CURRENT CAPABILITIES

- pipelines, redirections, semicolon-separated commands, and `&&`/`||`
- background jobs with `jobs`, `fg`, `bg`, and `wait`
- variable assignment and expansion, wildcard expansion, and command
  substitution with `$()`
- `if`, `while`, `for`, `case`, and shell-function definitions
- here-documents and aliases
- interactive history plus simple line editing and completion when run on a TTY

## INTERNAL LAYOUT

- `shell_parser.c` — tokenization and parse-tree construction
- `shell_execution.c` — process launching, pipelines, and job handling
- `shell_builtins.c` — built-in commands and shell state updates
- `shell_interactive.c` — line editing, prompt loop, and command history
- `shell_shared.h` — shared limits and data structures

## CURRENT BOUNDARIES

- The subsystem is private to `sh`; other tools should not depend on it
- Interactive features only apply when the shell is attached to a terminal
- Fixed ceilings remain in place: 8 commands per pipeline, 64 arguments per
  command, 16 jobs, 64 history entries, and 32 aliases/functions
- No process substitution, and shell-language compatibility is still narrower
  than `bash` or `dash`

## SEE ALSO

man, sh, project-layout, runtime
