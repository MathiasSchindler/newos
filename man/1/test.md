# TEST

## NAME

test - evaluate a conditional expression

## SYNOPSIS

```
test EXPRESSION
```

## DESCRIPTION

`test` evaluates EXPRESSION and exits 0 (true) or 1 (false). It is
equivalent to `[ EXPRESSION ]` but without the required closing `]`.

## CURRENT CAPABILITIES

**File tests:**

- `-e FILE` — FILE exists
- `-f FILE` — FILE exists and is a regular file
- `-d FILE` — FILE exists and is a directory
- `-h FILE`, `-L FILE` — FILE exists and is a symbolic link
- `-s FILE` — FILE exists and has size greater than zero
- `-r FILE` — FILE is readable
- `-w FILE` — FILE is writable
- `-x FILE` — FILE is executable
- `-t FD` — file descriptor FD is open on a terminal
- `-u FILE` — FILE has setuid bit set
- `-g FILE` — FILE has setgid bit set
- `-k FILE` — FILE has sticky bit set

**String tests:**

- `-n STRING` — STRING is non-empty
- `-z STRING` — STRING is empty
- `STRING1 = STRING2`, `STRING1 == STRING2` — strings are equal
- `STRING1 != STRING2` — strings are not equal
- `STRING1 < STRING2` — STRING1 is lexicographically less than STRING2
- `STRING1 > STRING2` — STRING1 is lexicographically greater than STRING2

**Integer comparisons:**

- `INT1 -eq INT2` — equal
- `INT1 -ne INT2` — not equal
- `INT1 -gt INT2` — greater than
- `INT1 -ge INT2` — greater than or equal
- `INT1 -lt INT2` — less than
- `INT1 -le INT2` — less than or equal

**File comparisons:**

- `FILE1 -nt FILE2` — FILE1 is newer than FILE2
- `FILE1 -ot FILE2` — FILE1 is older than FILE2
- `FILE1 -ef FILE2` — FILE1 and FILE2 refer to the same inode

**Logical operators:**

- `! EXPRESSION` — negation
- `EXPRESSION1 -a EXPRESSION2` — logical AND
- `EXPRESSION1 -o EXPRESSION2` — logical OR
- `( EXPRESSION )` — grouping

## OPTIONS

None.

## LIMITATIONS

- No `-N FILE` (file was modified since last read).
- Complex nested parenthesised expressions may hit parser edge cases.

## EXAMPLES

```
test -f /etc/passwd && echo "exists"
test -d src || mkdir src
test "$x" -gt 0
test -z "$VAR"
```

## SEE ALSO

[, expr, sh
