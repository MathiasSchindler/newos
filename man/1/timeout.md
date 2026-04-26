# TIMEOUT

## NAME

timeout - run a command with a time limit

## SYNOPSIS

```
timeout [--preserve-status] [-s SIGNAL|--signal SIGNAL]
	[-k DURATION|--kill-after DURATION] DURATION COMMAND [ARG ...]
```

## DESCRIPTION

timeout executes COMMAND with its ARGs and sends it a signal if it has not exited within DURATION. The default signal is TERM. An optional kill signal can be sent as a backstop after a further delay.

## CURRENT CAPABILITIES

- running a command with a configurable time limit
- sending a configurable signal on timeout (TERM by default)
- sending a follow-up KILL signal after an additional delay
- preserving the child process exit status when a timeout occurs

## OPTIONS

- `--preserve-status` — exit with the same status as the child even when a timeout occurred (instead of exit 124)
- `-s SIGNAL` / `--signal SIGNAL` — signal to send on timeout; SIGNAL may be a name (`TERM`, `HUP`, …) or a number
- `-k DURATION` / `--kill-after DURATION` — send SIGKILL after DURATION if the command is still running after the primary signal
- `DURATION` — time in seconds; may include suffix `s` (seconds), `m` (minutes), `h` (hours), `d` (days), or decimal fractions (e.g. `1.5`)

## LIMITATIONS

- no `--foreground` option for use in shell pipelines
- timeout accuracy depends on the platform timer resolution; sub-millisecond durations are not guaranteed

## EXAMPLES

- `timeout 30 long_script.sh` — kill after 30 seconds
- `timeout -s HUP 10 server` — send SIGHUP after 10 seconds
- `timeout -k 5 60 prog` — send TERM after 60 s, KILL after 65 s
- `timeout 0.5 curl http://slow/` — 500 ms HTTP timeout

## SEE ALSO

kill, sh, sleep
