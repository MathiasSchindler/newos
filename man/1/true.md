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

None.

## EXAMPLES

```
true
while true; do echo "looping"; sleep 1; done
command || true
```

## SEE ALSO

false, test
