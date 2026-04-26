# UPTIME

## NAME

uptime - show how long the system has been running

## SYNOPSIS

```
uptime [-p|--pretty] [-s|--since]
```

## DESCRIPTION

uptime prints the current time, how long the system has been running, and optionally the time the system was last booted. It reads uptime information from the platform interface.

## CURRENT CAPABILITIES

- default one-line summary with current time, uptime duration, and user count
- human-readable pretty-printed duration
- boot timestamp in ISO 8601 format

## OPTIONS

- `-p` / `--pretty` — print uptime in a human-readable form such as `up 3 hours, 25 minutes`
- `-s` / `--since` — print the date and time the system was last booted

## LIMITATIONS

- load average display depends on platform availability; may be absent on some systems
- user count reflects active login sessions; may differ from the reference `uptime` on some systems

## EXAMPLES

- `uptime` — standard one-line summary
- `uptime -p` — pretty duration only
- `uptime -s` — print boot time

## SEE ALSO

who, free, pstree, ps
