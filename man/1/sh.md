# SH

## NAME

sh - command shell for the newos userland

## SYNOPSIS

```text
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

- interactive command entry with in-session history
- one-shot execution with `-c` and script execution from a file
- pipelines, redirections, background jobs, and here-docs
- shell builtins including `cd`, `exit`, `jobs`, `fg`, `bg`, `history`,
  `export`, `unset`, `alias`, and `command -v`
- variable expansion, aliases, and shell functions in the current implementation

## OPTIONS

- `-c COMMAND` — execute `COMMAND` and exit
- `SCRIPT [ARG ...]` — read commands from `SCRIPT`; extra command-line
  arguments are currently accepted but have only limited script visibility

## LIMITATIONS

- it is not a full drop-in replacement for a mature POSIX shell
- job control and interactive convenience features remain basic compared with
  `bash` or `dash`
- script argument handling is still narrower than full POSIX `$1`, `$2`, and
  related behavior
- shell invocation options are intentionally minimal; login-shell startup and
  broader compatibility modes are not implemented

## EXAMPLES

```text
sh
sh -c 'echo hello | wc -c'
sh build-script.sh
printf 'echo hello\n' | sh
```

## SEE ALSO

man, shell, env, test
