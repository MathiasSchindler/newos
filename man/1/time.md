# TIME

## NAME

time - run a command and report elapsed time

## SYNOPSIS

```
time [-v] COMMAND [ARG ...]
```

## DESCRIPTION

`time` runs COMMAND with its arguments, waits for it to finish, prints timing information to standard error, and exits with the command's status.

Use `-v` or `--verbose` to include additional process-accounting counters when
the platform backend exposes them.

## OPTIONS

- `-v`, `--verbose` - include CPU utilization, page faults, context switches,
  and CPU migrations after the standard `real`, `user`, and `sys` lines.
- `-h`, `--help` - show usage.

## CURRENT CAPABILITIES

- execute a command through the platform process-spawn interface
- report monotonic wall-clock elapsed time as `real`
- report child process user and system CPU time where the platform exposes it
- report CPU utilization and selected rusage-style counters in verbose mode
- preserve the wrapped command's exit status
- report execution failures with conventional shell-style status values

## OUTPUT

Timing is printed to standard error as:

```
real SECONDS.MICROS
user 0.000000
sys 0.000000
```

Verbose mode appends:

```text
cpu PERCENT
minor_faults COUNT
major_faults COUNT
voluntary_context_switches COUNT
involuntary_context_switches COUNT
migrations COUNT
```

`cpu` is computed from child user plus system CPU time divided by elapsed wall
time, so multicore workloads can exceed 100%.

## LIMITATIONS

- elapsed timing uses the platform monotonic clock and is printed with six fractional digits
- user, system, and verbose accounting depends on platform wait/rusage support and may be zero on fallback platforms
- shell-reserved-word features such as pipeline timing are outside the standalone tool scope

## EXAMPLES

```
time make test
time -v make test
time sleep 1
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

timeout, sh, sleep, ps
