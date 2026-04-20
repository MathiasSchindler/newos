# INIT

## NAME

init - tiny init-style process supervisor

## SYNOPSIS

```sh
init [-nq] [-r DELAY] [-m COUNT] [-t PATH] [-e NAME=VALUE] [-c COMMAND]
     [PROGRAM [ARG ...]]
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
- attach the supervised program to a chosen console or stdio path with `-t`
- export child environment overrides with repeated `-e NAME=VALUE`
- sanitize the default `PATH` to `/bin:/usr/bin` and require absolute direct
  program paths to reduce command-hijack risk
- restart the managed child after exit
- bound crash loops with `-m`
- disable respawning for one-shot use with `-n`
- pause between restarts with `-r`
- keep status messages off stderr with `-q`

## OPTIONS

- `-n`, `--no-respawn` - run the child once and exit with its status
- `-q`, `--quiet` - suppress supervisor status messages
- `-r DELAY`, `--restart-delay DELAY` - wait before respawning; accepts the same
  duration syntax used by other tools such as `100ms`, `1s`, or `2m`
- `-m COUNT`, `--max-restarts COUNT` - stop respawning after `COUNT` restarts;
  `0` means “run once unless `-n` already does”
- `-t PATH`, `--console PATH` - connect the child’s stdin/stdout/stderr to a
  console or log path such as `/dev/console`; use `-` to reuse the current
  stdio streams
- `-e NAME=VALUE`, `--setenv NAME=VALUE` - export an environment override to
  the supervised child; may be provided more than once
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
init -n /bin/sh -c 'echo boot smoke test'
init -r 500ms /bin/sh
init -t /dev/console -e TERM=linux -e PATH=/bin:/usr/bin /bin/sh
init -m 3 -r 1s -c 'echo booting; exit 1'
init -q -c 'while true; do date; sleep 5; done'
```

## SEE ALSO

sh, watch, timeout, pstree
