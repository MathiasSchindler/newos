# HOSTNAME

## NAME

hostname - display or set the system hostname

## SYNOPSIS

```text
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

## EXAMPLES

```text
hostname
hostname newos-dev
```

## SEE ALSO

uname, whoami
