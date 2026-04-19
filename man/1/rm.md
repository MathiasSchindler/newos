# RM

## NAME

rm - remove files and directories

## SYNOPSIS

```text
rm [-f] [-i] [-I] [-v] [-d] [-r] [--interactive=WHEN] [--preserve-root|--no-preserve-root] path ...
```

## DESCRIPTION

`rm` removes files and, when requested, directory trees. It includes a small set
of interactive and safety-oriented behaviors.

## CURRENT CAPABILITIES

- remove one or more regular files
- remove empty directories with `-d`
- remove directory trees recursively with `-r`
- prompt before each removal with `-i` or in a safer once-per-command mode
- protect `/` from recursive deletion by default
- ignore missing paths with `-f` and print removed names with `-v`

## OPTIONS

- `-f` ignore missing files and suppress prompts
- `-i` prompt before every removal
- `-I`, `--interactive=WHEN` choose prompting behavior for common safer modes
- `-d` remove empty directories
- `-r` remove directory trees recursively
- `--preserve-root` keep the default safeguard against recursive removal of `/`
- `--no-preserve-root` disable that safeguard
- `-v` print removed paths

## LIMITATIONS

- removals are permanent; there is no trash or undelete layer
- interactive handling is intentionally simpler than full GNU `rm` in edge
  cases involving many write-protected files or deep directory trees

## EXAMPLES

```text
rm file.txt
rm -r build/tmp
rm -iv old-config
```

## SEE ALSO

rmdir, mkdir, mv
