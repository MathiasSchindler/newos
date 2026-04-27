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
- If the identity cannot be resolved, exits with status 1 and prints an error to standard error.
- Output depends on the platform identity backend; minimal freestanding
  environments may only know numeric IDs or a small static account set.
- It reports the effective user identity only; use `id` for group lists or
  broader identity details.

## EXAMPLES

```
whoami
```

## SEE ALSO

id
