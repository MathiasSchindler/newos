# INIT

## NAME

init - tiny init-style process supervisor

## SYNOPSIS

```sh
init [-nq] [-r DELAY] [-c COMMAND] [PROGRAM [ARG ...]]
```

## DESCRIPTION

`init` runs a command under a small supervisor loop. If the child exits, `init`
can restart it after a configurable delay. With no command given, it launches
`/bin/sh`.

This is intended as a compact early-userspace or container-style init tool
rather than a full service manager.

## CURRENT CAPABILITIES

- launch a default shell when no explicit program is given
- run a shell command string with `-c`
- restart the managed child after exit
- disable respawning for one-shot use with `-n`
- pause between restarts with `-r`
- keep status messages off stderr with `-q`

## OPTIONS

- `-n`, `--no-respawn` - run the child once and exit with its status
- `-q`, `--quiet` - suppress supervisor status messages
- `-r DELAY`, `--restart-delay DELAY` - wait before respawning; accepts the same
  duration syntax used by other tools such as `100ms`, `1s`, or `2m`
- `-c COMMAND`, `--command COMMAND` - run `sh -c COMMAND` under supervision
- `-h`, `--help` - show usage information

## LIMITATIONS

- manages exactly one foreground child process
- no service units, runlevels, dependency graph, or daemon configuration files
- no explicit shutdown, reboot, or signal-forwarding policy yet
- not a replacement for a full `systemd`, SysV init, or OpenRC style init
- zombie reaping is limited to the directly managed child

## EXAMPLES

```sh
init
init -n sh -c 'echo boot smoke test'
init -r 500ms /bin/sh
init -q -c 'while true; do date; sleep 5; done'
```

## SEE ALSO

sh, watch, timeout, pstree
