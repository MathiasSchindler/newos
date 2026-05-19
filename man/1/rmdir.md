# RMDIR

## NAME

rmdir - remove empty directories

## SYNOPSIS

```
rmdir [-p] [-v] [--ignore-fail-on-non-empty] directory ...
```

## DESCRIPTION

`rmdir` removes empty directories. It can also walk upward and remove empty
parents when requested.

## CURRENT CAPABILITIES

- remove empty directories
- remove empty parent chains with `-p`
- verbose output with `-v`
- optionally ignore non-empty targets

## OPTIONS

- `-p` remove parent directories when they become empty
- `-v` print each removed directory
- `--ignore-fail-on-non-empty` suppress failure for non-empty paths

## LIMITATIONS

- it is specifically for empty-directory removal; use `rm -r` for trees
- behavior is intentionally focused on the core Unix workflow
- no verbose parent-by-parent compatibility quirks beyond the documented flags
- no transactional rollback if `-p` removes some parents and then a later parent
  cannot be removed

## EXAMPLES

```
rmdir empty-dir
rmdir -p a/b/c
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

rm, mkdir
