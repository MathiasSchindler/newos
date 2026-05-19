# USERS

## NAME

users - print usernames of logged-in users

## SYNOPSIS

```
users [-s] [-u] [-c] [-l] [--host HOST] [--terminal TTY] [--since EPOCH] [USER ...]
```

## DESCRIPTION

users prints the usernames of all users currently logged in to the system, derived from active login sessions.

## CURRENT CAPABILITIES

- listing usernames from active sessions
- sorting the output alphabetically
- deduplicating to show each user at most once
- printing only the count of logged-in users
- filtering sessions by username, host, terminal, or login time
- showing terminal, host, and login-time fields in long output

## OPTIONS

- `-s` — sort usernames alphabetically
- `-u` — print only unique usernames (implies `-s`)
- `-c` — print only the count of logged-in users
- `-l` — long output with username, terminal, login time, and host
- `--host HOST` — include only sessions from HOST
- `--terminal TTY`, `--tty TTY` — include only sessions on TTY
- `--since EPOCH` — include only sessions whose login time is at or after EPOCH
- `USER ...` — include only sessions for the named users

## LIMITATIONS

- session data depends on the platform login record interface
- no idle-time field is available through the shared platform interface
- historical login database queries are not implemented; filters apply to the
  current active session set
- results may be sparse until `login` and the platform session database grow
  fuller accounting support

## EXAMPLES

- `users` — list all logged-in usernames
- `users -u` — list unique usernames
- `users -c` — print the number of logged-in users
- `users -l` — include terminal, login time, and host details
- `users --since @0 alice` — show Alice's current sessions logged after epoch 0

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

who, groups, id, w
