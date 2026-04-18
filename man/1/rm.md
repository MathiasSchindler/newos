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

- remove regular files
- remove directories with `-d`
- recursive removal with `-r`
- interactive prompting modes
- root-preservation safety checks
- verbose reporting of removed paths

## OPTIONS

- `-f` ignore missing files and suppress prompts
- `-i`, `-I`, `--interactive=WHEN` choose prompting behavior
- `-d` remove empty directories
- `-r` remove directory trees recursively
- `-v` print removed paths

## LIMITATIONS

- this implementation favors safety and the project workflow over every historical compatibility corner case
- interactive details are intentionally simpler than large host implementations

## EXAMPLES

```text
rm file.txt
rm -r build/tmp
rm -iv old-config
```

## SEE ALSO

rmdir, mkdir, mv
