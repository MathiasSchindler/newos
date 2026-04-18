# BC

## NAME

bc - arbitrary precision calculator

## SYNOPSIS

bc [expression]

## DESCRIPTION

bc evaluates arithmetic expressions with arbitrary precision. An expression may be provided directly on the command line or interactively on standard input. Supports integers and fixed-decimal fractions, variables, and basic arithmetic.

## CURRENT CAPABILITIES

- arithmetic operators: `+`, `-`, `*`, `/`, `%`, `^` (power)
- comparison operators: `==`, `!=`, `<`, `<=`, `>`, `>=`
- assignment: `var = expr`
- built-in functions: `sqrt(x)`, `length(x)`, `scale(x)`
- the special variable `scale` controls the number of decimal digits
- multi-line expressions via standard input
- inline expression via command-line argument

## OPTIONS

bc accepts no flags other than an expression argument and `--help`.

## LIMITATIONS

- no `bc` standard library functions (`s`, `c`, `a`, `e`, `j`, `l`) as defined in POSIX
- no `-l` (math library) flag
- no `define` for user-defined functions
- no `for`, `while`, or `if` control flow statements
- output base (`obase`) and input base (`ibase`) variables are not supported

## EXAMPLES

- `bc <<< "2^32"` — compute a power
- `bc <<< "scale=10; 22/7"` — pi approximation to 10 decimal places
- `echo "3 * (4 + 5)" | bc` — pipeline calculation
- `bc` — interactive session

## SEE ALSO

expr, awk, seq
