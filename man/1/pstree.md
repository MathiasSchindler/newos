# PSTREE

## NAME

pstree - display processes in a tree

## SYNOPSIS

```
pstree [-A] [-n] [-p] [-u] [PID]
```

## DESCRIPTION

pstree reads the current process list and displays parent-child relationships as
an indented tree. By default it prefers Unicode line-drawing characters for a
cleaner display; `-A` forces plain ASCII branches.

## CURRENT CAPABILITIES

- tree display of all processes
- rooting the tree at a specific PID
- optional PID display with `-p`
- optional ASCII branch rendering with `-A`
- user annotations on ownership transitions with `-u`
- name-first or numeric PID-first sibling ordering

## OPTIONS

| Flag | Description |
|------|-------------|
| `-A` | Use plain ASCII branch characters instead of Unicode line drawing. |
| `-n` | Sort siblings numerically by PID instead of by process name. |
| `-p` | Show PIDs next to each process name. |
| `-u` | Show the username when it differs from the parent process (and on the root of the displayed tree). |
| `PID` | Display only the subtree rooted at the given process ID. |

## LIMITATIONS

- process names are truncated to the platform limit for the comm field
- no compact same-name grouping (e.g. `sshd(3)`) as in GNU/BSD pstree
- no thread display (`-T` flag)
- no thread display or same-name compaction yet

## EXAMPLES

- `pstree` — show the full process tree
- `pstree 1` — show the subtree rooted at PID 1
- `pstree -p 1` — show the subtree rooted at PID 1 with PIDs
- `pstree -A -u` — show an ASCII tree with user transitions

## SEE ALSO

ps, kill, uptime
