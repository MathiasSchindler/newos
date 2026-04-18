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

None.

## EXAMPLES

```
false
false || echo "always printed"
if false; then echo "never"; fi
```

## SEE ALSO

true, test
