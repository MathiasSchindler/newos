# TRUE

## NAME

true - return a successful exit status

## SYNOPSIS

```
true
```

## DESCRIPTION

`true` does nothing and exits with status 0 (success). It is commonly used
as a no-op in shell scripts or as a loop condition.

## CURRENT CAPABILITIES

- Exits with status 0 unconditionally.

## OPTIONS

None. All arguments are silently ignored.

## LIMITATIONS

- All arguments are ignored; GNU-style `--help` and `--version` special cases
  are not implemented.
- There is intentionally no diagnostic mode, timing mode, or shell-builtin
  integration beyond returning status 0.
- Portability-sensitive scripts should not rely on this external command
  printing anything, even for unexpected operands.

## EXAMPLES

```
true
while true; do echo "looping"; sleep 1; done
command || true
```

## SEE ALSO

false, test
