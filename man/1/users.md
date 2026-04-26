# USERS

## NAME

users - print usernames of logged-in users

## SYNOPSIS

```
users [-s] [-u] [-c]
```

## DESCRIPTION

users prints the usernames of all users currently logged in to the system, derived from active login sessions.

## CURRENT CAPABILITIES

- listing usernames from active sessions
- sorting the output alphabetically
- deduplicating to show each user at most once
- printing only the count of logged-in users

## OPTIONS

- `-s` — sort usernames alphabetically
- `-u` — print only unique usernames (implies `-s`)
- `-c` — print only the count of logged-in users

## LIMITATIONS

- no arguments accepted; cannot query users for a different host or time
- session data depends on the platform login record interface

## EXAMPLES

- `users` — list all logged-in usernames
- `users -u` — list unique usernames
- `users -c` — print the number of logged-in users

## SEE ALSO

who, groups, id, w
