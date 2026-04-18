# ID

## NAME

id - print user and group identity information

## SYNOPSIS

```
id [-u|-g|-G] [-n] [USER]
```

## DESCRIPTION

`id` prints the real user ID, group ID, and supplementary group list for the
current process or for the named USER.

## CURRENT CAPABILITIES

- Full identity summary: `uid=N(name) gid=N(name) groups=N(name),...`
- Print only UID with `-u`
- Print only GID with `-g`
- Print all group IDs with `-G`
- Print names instead of numeric IDs with `-n`
- Look up identity for a named user

## OPTIONS

- `-u` — print only the effective user ID (or name with `-n`)
- `-g` — print only the effective group ID (or name with `-n`)
- `-G` — print all group IDs (or names with `-n`) separated by spaces
- `-n` — print names instead of numeric IDs (requires `-u`, `-g`, or `-G`)
- `USER` — look up identity for USER instead of the current process

## LIMITATIONS

- `-n` cannot be used without one of `-u`, `-g`, or `-G`.
- Supplementary group list is capped at 256 entries.
- No `-r` (real ID) or `-e` (effective ID) distinction flags.

## EXAMPLES

```
id
id -u
id -un
id -G -n
id root
```

## SEE ALSO

whoami, groups
