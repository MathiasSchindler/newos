# GROUPS

## NAME

groups - print the groups a user belongs to

## SYNOPSIS

```
groups [-n] [-d] [-p] [USER ...]
```

## DESCRIPTION

groups prints the names (or GIDs) of all groups the specified user(s) belong to. With no USER argument it queries the current user.

## CURRENT CAPABILITIES

- listing group names for the current user or named users
- printing numeric GIDs instead of names
- restricting output to the primary group
- prefixing output with the username when multiple users are queried

## OPTIONS

- `-n` — print numeric group IDs instead of group names
- `-d` — print only the primary (default) group
- `-p` — prefix each output line with `username:` (enabled automatically when more than one USER is given)

## LIMITATIONS

- group membership is read from the system group database; supplementary groups assigned at login session level may not always appear
- no support for reading from an alternate group file
- no network directory service, NSS module, or cache integration is provided
- output is intentionally compact and does not include numeric IDs unless the
  platform name lookup already exposes them elsewhere

## EXAMPLES

- `groups` — list groups for the current user
- `groups alice` — list groups for alice
- `groups -n` — list numeric GIDs
- `groups -d alice bob` — show primary group for alice and bob

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

id, who, users
