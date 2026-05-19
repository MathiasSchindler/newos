# CLEAR

## NAME

clear - clear the terminal screen

## SYNOPSIS

```
clear
clear -h
```

## DESCRIPTION

`clear` writes a standard ANSI terminal reset sequence that moves the cursor to
the home position and clears the visible screen. On terminals that support the
extended clear sequence, it also requests clearing the scrollback buffer.

## CURRENT CAPABILITIES

- clear the visible terminal area
- move the cursor to the upper-left corner
- request scrollback clearing on ANSI-compatible terminals
- provide a minimal usage summary with `-h` or `--help`

## OPTIONS

- `-h`, `--help`  show a short usage summary

## LIMITATIONS

- this is intentionally a small ANSI/VT100-style implementation rather than a full terminfo-aware utility
- behavior depends on terminal escape-sequence support; on very minimal or non-ANSI consoles the screen may not clear fully
- it does not provide compatibility options or alternate modes

## EXAMPLES

```
clear
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

printf, sh
