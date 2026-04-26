# PRINTENV

## NAME

printenv - print environment variables

## SYNOPSIS

```
printenv [-0] [-n] [-q] [NAME ...]
```

## DESCRIPTION

printenv prints the values of the environment variables named by its arguments. With no arguments it prints all variables and their values in `NAME=VALUE` form.

## CURRENT CAPABILITIES

- printing all environment variables
- printing the value of specific named variables
- NUL-terminated output mode
- printing variable names only (without values)
- quiet mode that suppresses errors for unset variables

## OPTIONS

- `-0` — separate entries with NUL instead of newline
- `-n` — print only variable names, not their values
- `-q` — quiet mode: exit without error when a named variable is not set (normally exits non-zero)

## LIMITATIONS

- does not support the `env`-style `VAR=value COMMAND` invocation
- no `-u` (unset) flag

## EXAMPLES

- `printenv` — list all environment variables
- `printenv PATH` — print the value of PATH
- `printenv -0 HOME USER` — print two variables NUL-separated
- `printenv -q MAYBE_UNSET` — exit 0 even if the variable is unset

## SEE ALSO

env, export, sh
