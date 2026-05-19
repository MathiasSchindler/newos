# HOSTNAME

## NAME

hostname - display or set the system hostname

## SYNOPSIS

```
hostname [NAME]
```

## DESCRIPTION

`hostname` prints the current host name when called without arguments and
attempts to set it when a new name is provided.

## CURRENT CAPABILITIES

- print the current hostname
- request a hostname change with a positional name argument

## OPTIONS

This command currently uses a simple positional interface rather than a large
flag set.

## LIMITATIONS

- changing the hostname usually requires suitable system privileges
- behavior depends on the current platform backend and host permissions
- domain, FQDN, alias, and DNS lookup modes from GNU/BSD `hostname` are not
  implemented.
- no persistent configuration file is updated; setting the name only asks the
  running platform to change its current hostname.

## EXAMPLES

```
hostname
hostname newos-dev
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

uname, whoami
