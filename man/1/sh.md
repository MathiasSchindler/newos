# SH

## NAME

sh - command shell for the newos userland

## SYNOPSIS

```
sh
sh -c COMMAND
sh SCRIPT [ARG ...]
```

## DESCRIPTION

`sh` provides the project's interactive and scriptable shell environment. With
no arguments it reads commands from standard input, using interactive mode when
stdin is a terminal. With `-c` it executes a single command string, and with a
script path it runs the file non-interactively.

## CURRENT CAPABILITIES

- interactive command entry with in-session history, cursor movement, and basic
  line editing conveniences
- one-shot execution with `-c` and script execution from a file
- pipelines, redirections, background jobs, and here-docs
- shell builtins including `cd`, `exit`, `jobs`, `fg`, `bg`, `history`,
  `export`, `unset`, `alias`, `set`, `shift`, and `command -v`
- variable expansion, aliases, and shell functions in the current implementation
- script and `-c` argument visibility through `$0`, `$1` ... `${10}`, `$#`,
  `$*`, and `$@`
- interactive Tab completion for command names and path arguments

## OPTIONS

- `-c COMMAND` — execute `COMMAND` and exit
- `SCRIPT [ARG ...]` — read commands from `SCRIPT`; extra command-line
  arguments are exposed through the usual positional-parameter expansions

## INTERACTIVE CONVENIENCES

- `Tab` completes command names and path fragments in interactive mode
- the interactive prompt shows the current working directory by default
- `Up` / `Down` browse the current session history
- `Left` / `Right`, `Home`, and `End` move the cursor within the line
- `Ctrl-A`, `Ctrl-E`, `Ctrl-U`, `Ctrl-K`, `Ctrl-W`, and `Backspace` provide
  quick line editing
- if raw terminal mode is unavailable, common control-key edits and unique Tab
  completions are still replayed sensibly from scripted tty input

## LIMITATIONS

- it is not a full drop-in replacement for a mature POSIX shell
- job control and interactive convenience features remain basic compared with `bash` or `dash`
- positional parameter handling is intentionally compact and does not yet match every quoting edge case of a full POSIX shell
- shell invocation options are intentionally minimal; login-shell startup, rc-file loading, and broader compatibility modes are not implemented yet

## EXAMPLES

```
sh
sh -c 'echo hello | wc -c'
sh build-script.sh
sh -c 'echo "$0 -> $1"' name value
printf 'echo hello\n' | sh
```

## SEE ALSO

man, shell, env, test
