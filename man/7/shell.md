# SHELL

## NAME

shell - the sh command and its underlying shell subsystem

## DESCRIPTION

The shell component consists of the sh tool (src/tools/sh.c) and four shared source modules that implement parsing, execution, built-in commands, and interactive line editing. The shared modules live in src/shared and are compiled into sh; they are not linked into any other tool.

## CURRENT CAPABILITIES

- POSIX-style command parsing including pipelines, redirections, and semicolons
- AND/OR lists (&&, ||) and background jobs (&)
- here-documents
- variable assignment and expansion
- command substitution via $()
- if, while, for, case, and function definitions
- built-in commands: cd, export, unset, exit, source, alias, jobs, fg, bg, wait, and others
- interactive line editing with history (up to 64 entries) and simple completion
- up to 16 concurrent background jobs
- up to 32 user-defined aliases and 32 shell functions

## INTERFACES

- src/shared/shell_shared.h defines shared constants and data structures
- src/shared/shell_parser.c tokenises and parses command lines into ShCommand trees
- src/shared/shell_execution.c forks, execs, and manages pipelines and job control
- src/shared/shell_builtins.c implements the built-in command table
- src/shared/shell_interactive.c handles the read-line loop and history

## LIMITATIONS

- Arithmetic expansion is limited; bc is the recommended workaround for complex expressions
- No process substitution (<(...) or >(...))
- History and line editing are only active in interactive mode; non-interactive scripts run without them
- SH_MAX_ARGS is capped at 64; SH_MAX_COMMANDS per pipeline at 8

## SEE ALSO

man, sh, project-layout, runtime
