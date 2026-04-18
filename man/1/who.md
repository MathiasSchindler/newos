# WHO

## NAME

who - show who is logged in

## SYNOPSIS

who [-H] [-b] [-q]

## DESCRIPTION

who lists the users currently logged in to the system. Each entry shows the username, terminal, and login time. With `-b` it shows the last boot time instead.

## CURRENT CAPABILITIES

- listing active login sessions with username, terminal, and login time
- printing a column header row
- showing last system boot time
- quick mode listing only usernames

## OPTIONS

- `-H` — print a header line (`NAME LINE TIME`) above the session list
- `-b` — print the date and time of the last system boot instead of the session list
- `-q` — quick mode: print only usernames on one line, followed by a `# users=N` summary

## LIMITATIONS

- session information is read from the platform login record interface; availability depends on the host OS
- no support for showing idle time, process, or host fields as in GNU who
- no `am i` / `mom likes` invocation

## EXAMPLES

- `who` — list logged-in users
- `who -H` — list with column headers
- `who -q` — list usernames only
- `who -b` — show last boot time

## SEE ALSO

users, groups, uptime, id
