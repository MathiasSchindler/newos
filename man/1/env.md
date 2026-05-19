# ENV

## NAME

env - set environment and run a command

## SYNOPSIS

```
env [-i] [-0] [-u NAME] [NAME=VALUE ...] [COMMAND [ARG ...]]
```

## DESCRIPTION

The env tool prints the current environment or runs a command with modified environment variables. It can start from an empty environment or unset selected names first.

## CURRENT CAPABILITIES

- print the current environment when no command is given
- start with an empty environment using `-i`
- unset selected variables with `-u`
- apply temporary `NAME=VALUE` overrides for one command
- emit NUL-delimited output with `-0`

## OPTIONS

| Flag | Description |
|------|-------------|
| `-i` | Start with an empty environment. |
| `-0` | Delimit output entries with NUL instead of newline. |
| `-u NAME` | Unset the named variable before printing or running a command. |
| `NAME=VALUE` | Set or override an environment variable for this invocation. |

## LIMITATIONS

- At most 64 `-u NAME` unsets are supported per invocation.
- The GNU `-S` / `--split-string` option is not implemented.
- No `-C DIR`, signal-disposition controls, or NUL-delimited environment output
  are implemented.
- Assignment parsing is intentionally simple; shell quoting and expansion must
  be performed by the caller's shell before `env` runs.

## EXAMPLES

```
env
env -i PATH=/bin sh -c "echo $PATH"
env -u HOME printenv
```

## NOTES

- **Important:** variable assignments made by env affect only the invoked command.

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

sh, printenv, export
