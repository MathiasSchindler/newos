# WHOAMI

## NAME

whoami - print the current user name

## SYNOPSIS

```
whoami
```

## DESCRIPTION

`whoami` prints the login name of the current user to standard output,
equivalent to `id -un`.

## CURRENT CAPABILITIES

- Prints the effective username of the running process.

## OPTIONS

None.

## LIMITATIONS

- No options are accepted.
- If the identity cannot be resolved, exits with status 1 and prints an error
  to standard error.

## EXAMPLES

```
whoami
```

## SEE ALSO

id
