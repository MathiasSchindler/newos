# AR

## NAME

**ar** - create, replace, list, print, and extract archive members

## SYNOPSIS

```
ar [r|q|t|p|x][c][s][v] ARCHIVE [FILE ...]
```

## DESCRIPTION

`ar` works with traditional Unix archive files such as static libraries.

Implemented operations:

- **r** — replace or add members
- **q** — append members
- **t** — list members
- **p** — write members to standard output
- **x** — extract members into the current directory
- **v** — verbose output
- **c**, **s** — accepted for compatibility

Long member names are written in a portable extended-name form.

## EXAMPLES

```
ar rcs libdemo.a foo.o bar.o
ar t libdemo.a
ar x libdemo.a foo.o
```

## LIMITATIONS

This implementation focuses on the common archive workflow used for static libraries and does not yet provide the full GNU `ar` option surface.
