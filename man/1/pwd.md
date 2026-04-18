# PWD

## NAME

pwd - print the current working directory

## SYNOPSIS

```text
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

## EXAMPLES

```text
pwd
pwd -P
```

## SEE ALSO

cd, realpath, ls
