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

- This implementation focuses on the common archive workflow used for static
  libraries and does not yet provide the full GNU `ar` option surface.
- Thin archives, MRI scripts, deterministic timestamp controls, member move
  operations, and plugin-aware archive handling are not implemented.
- The `s` modifier is accepted for compatibility, but full symbol-index
  maintenance is still narrower than mature `ar`/`ranlib` toolchains.
- Archive metadata preservation is intentionally basic; ownership, timestamps,
  and long-name edge cases may not match every host implementation.

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

