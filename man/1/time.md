# TIME

## NAME

time - run a command and report elapsed time

## SYNOPSIS

```
time COMMAND [ARG ...]
```

## DESCRIPTION

`time` runs COMMAND with its arguments, waits for it to finish, prints timing information to standard error, and exits with the command's status.

## CURRENT CAPABILITIES

- execute a command through the platform process-spawn interface
- report wall-clock elapsed time as `real`
- preserve the wrapped command's exit status
- report execution failures with conventional shell-style status values

## OUTPUT

Timing is printed to standard error as:

```
real SECONDS.00
user 0.00
sys 0.00
```

## LIMITATIONS

- timing resolution is currently seconds because the shared platform API exposes epoch seconds
- user and system CPU accounting are not available yet and are reported as zero
- shell-reserved-word features such as pipeline timing are outside the standalone tool scope

## EXAMPLES

```
time make test
time sleep 1
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

timeout, sh, sleep, ps
