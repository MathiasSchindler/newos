# PSTREE

## NAME

pstree - display processes in a tree

## SYNOPSIS

pstree [-u] [PID]

## DESCRIPTION

pstree reads the current process list and displays it as a tree rooted at PID (or at the system init process if no PID is given). Parent-child relationships are shown by indented branches.

## CURRENT CAPABILITIES

- tree display of all processes
- rooting the tree at a specific PID
- showing user transitions between parent and child processes

## OPTIONS

- `-u` — show the username when a process is owned by a different user than its parent
- `PID` — display only the subtree rooted at the given process ID

## LIMITATIONS

- process names are truncated to the platform limit for the comm field
- no compact same-name grouping (e.g. `sshd(3)`) as in GNU/BSD pstree
- no thread display (`-T` flag)
- no PID display (`-p` flag)
- no ASCII/Unicode branch-character selection

## EXAMPLES

- `pstree` — show the full process tree
- `pstree 1` — show the subtree rooted at PID 1
- `pstree -u` — show user transitions in the tree

## SEE ALSO

ps, kill, uptime
