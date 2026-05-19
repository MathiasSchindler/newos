# RG

## NAME

rg - short command name for ripgrep

## SYNOPSIS

```
rg [OPTIONS] PATTERN [PATH ...]
rg --files [OPTIONS] [PATH ...]
```

## DESCRIPTION

`rg` is the short entry point for `ripgrep`. It accepts the same options and behavior as `ripgrep`.

## LIMITATIONS

- Same limitations as `ripgrep`; this is an alias-style entry point, not a
  separate implementation.
- No additional `rg`-specific configuration, aliases, or compatibility behavior
  is provided beyond the shared `ripgrep` option parser.
- Upstream ripgrep configuration files, ignore-file discovery, JSON output, and
  parallel walker behavior are absent for the same reasons described in
  `ripgrep`.

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

ripgrep, grep, find, output-style
