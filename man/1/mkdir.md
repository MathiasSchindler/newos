# MKDIR

## NAME

mkdir - create directories

## SYNOPSIS

```text
mkdir [-p] [-v] [-m mode] directory ...
```

## DESCRIPTION

`mkdir` creates one or more directories and can optionally create missing
parents and apply an explicit octal mode.

## CURRENT CAPABILITIES

- create one or more directories
- create parent directories with `-p`
- verbose creation messages with `-v`
- set an explicit mode with `-m`

## OPTIONS

- `-p` create parent directories as needed
- `-v` print a line for each created directory
- `-m MODE` apply an octal permission mode

## LIMITATIONS

- mode handling is numeric/octal-oriented
- advanced host-specific permission semantics are not the focus

## EXAMPLES

```text
mkdir work
mkdir -p build/tmp/cache
mkdir -m 700 private-dir
```

## SEE ALSO

rmdir, rm, chmod
