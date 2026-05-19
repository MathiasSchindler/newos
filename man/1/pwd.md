# PWD

## NAME

pwd - print the current working directory

## SYNOPSIS

```
pwd [-L|-P]
```

## DESCRIPTION

`pwd` reports the shell's current working directory. It can use logical or
physical path resolution depending on the selected mode.

## CURRENT CAPABILITIES

- print the current directory path
- logical mode with `-L`
- physical mode with `-P`

## OPTIONS

- `-L` prefer the logical path
- `-P` resolve and print the physical path

## LIMITATIONS

- the command is intentionally small and only implements the core path modes
- behavior depends on the platform layer's current directory support
- logical `-L` output depends on a trustworthy `PWD` environment value; unusual
  directory renames or symlink changes may require `-P`.
- no shell-builtin state is shared with `sh`; this external command only sees
  its process environment and platform cwd.

## EXAMPLES

```
pwd
pwd -P
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

cd, realpath, ls
