# WATCH

## NAME

watch - run a command repeatedly at a fixed interval

## SYNOPSIS

```
watch [-n INTERVAL] [-c COUNT] [-t] COMMAND [ARG ...]
```

## DESCRIPTION

`watch` runs COMMAND repeatedly and pauses between runs. When writing to a
terminal it prints a small header showing the interval, the command being run,
and the current time before each refresh. This makes it useful for quickly
monitoring process lists, resource output, test status, or other command output
that changes over time.

## CURRENT CAPABILITIES

- repeat a command until interrupted
- choose the refresh interval with `-n`
- stop automatically after a fixed number of runs with `-c`
- suppress the header/title block with `-t`
- preserve the exit status from the final command run

## OPTIONS

- `-n INTERVAL` / `--interval INTERVAL` - set the delay between runs; accepts
  plain seconds as well as suffixes such as `ms`, `s`, `m`, `h`, and `d`
- `-c COUNT` / `--count COUNT` - stop after COUNT executions instead of running
  forever
- `-t` / `--no-title` - suppress the header and screen-clearing title block

## LIMITATIONS

- no difference highlighting between runs
- no keyboard controls such as pause, quit, or on-change execution
- no shell-style parsing of a single quoted command string; pass the command and its arguments as separate argv entries

## EXAMPLES

```
watch ps
watch -n 0.5 free
watch -c 5 -t sh -c "date"
```

## SEE ALSO

sleep, timeout, ps, sh
