# FALSE

## NAME

false - return a failing exit status

## SYNOPSIS

```
false
```

## DESCRIPTION

`false` does nothing and exits with status 1 (failure). It is commonly used
as a no-op placeholder or to force a failing condition in shell scripts.

## CURRENT CAPABILITIES

- Exits with status 1 unconditionally.

## OPTIONS

None. All arguments are silently ignored.

## LIMITATIONS

- All arguments are ignored; GNU-style `--help` and `--version` special cases
  are not implemented.
- There is intentionally no diagnostic mode, timing mode, or shell-builtin
  integration beyond returning status 1.
- Portability-sensitive scripts should not rely on this external command
  printing anything, even for unexpected operands.

## EXAMPLES

```
false
false || echo "always printed"
if false; then echo "never"; fi
```

## SEE ALSO

true, test
