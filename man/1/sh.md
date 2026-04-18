# SH

## NAME

sh - command shell for the newos userland

## SYNOPSIS

```text
sh
sh SCRIPT [ARG ...]
```

## DESCRIPTION

`sh` provides the project's interactive and scriptable shell environment. It is
used to run pipelines, builtins, and external tools from the repository userland.

## CURRENT CAPABILITIES

- interactive command entry
- shell scripts and non-interactive execution
- pipelines and standard redirections
- builtins for common shell workflow
- variable expansion, aliases, and here-doc support in the current implementation

## OPTIONS

The shell currently focuses on invocation as an interactive shell or with a
script path. Its command-line flag surface is intentionally small.

## LIMITATIONS

- it is not a full drop-in replacement for a mature POSIX shell
- advanced job control, completion, and history behavior are still limited
- some shell-language edge cases and compatibility details remain narrower than `bash` or `dash`

## EXAMPLES

```text
sh
sh build-script.sh
printf 'echo hello\n' | sh
```

## SEE ALSO

man, shell, env, test
