# BC

## NAME

bc - line-oriented calculator with decimal arithmetic and variables

## SYNOPSIS

```
bc [expression]
```

## DESCRIPTION

`bc` evaluates arithmetic expressions supplied either as a single command-line
argument or through standard input. It is now a practical scripting calculator
for the newos userland, covering arbitrary-precision integer arithmetic,
scale-aware decimal arithmetic, variables, comparisons, boolean logic, base
conversion, and simple control flow.

Input is line-oriented. Separate expressions with newlines or semicolons; each
non-assignment result is written on its own line. In stdin mode, `#`, `//`,
and `/* ... */` comments are ignored.

## CURRENT CAPABILITIES

- arbitrary-precision integer literals and fixed-scale decimal literals
- arithmetic operators `+`, `-`, `*`, `/`, `%`, and `^`
- comparison operators `==`, `!=`, `<`, `<=`, `>`, and `>=`
- boolean operators `!`, `&&`, and `||`
- parentheses for grouping
- unary `+` and unary `-`
- variable assignment and reuse within the current input
- the special variables `scale` (division precision) and `last` (previous result)
- base conversion via `ibase` and `obase` (2 through 16)
- built-in functions `sqrt(x)`, `length(x)`, and `scale(x)`
- `if`, `while`, and `for` statements with `{}` blocks
- explicit output with the `print` keyword
- multiple expressions via standard input, separated by newline or `;`
- inline expression via command-line argument

## OPTIONS

`bc` accepts an optional `-l` mode plus an expression argument and `--help`.

- `-l` enables higher-precision math mode and predefines the constants `pi` and
  `e`

## LIMITATIONS

- arithmetic is backed by the shared signed big-number primitives in
  `src/shared/bignum.{c,h}`, so integer precision is large but still bounded by
  the fixed-capacity implementation (roughly 1150 decimal digits)
- decimal precision is controlled by `scale` and currently capped at 18
  fractional digits
- remainder is primarily useful with whole-number operands
- `-l` does not yet provide the full traditional POSIX math library function
  set; it mainly enables higher default precision and useful constants
- no `define`, `auto`, arrays, or string values
- no `break` or `continue` flow-control statements

## EXAMPLES

```sh
echo "3 * (4 + 5)" | bc
printf 'scale=4; 22 / 7\n' | bc
printf 'radius=12; radius^2\n' | bc
printf '5 + 7; last * 2\n' | bc
printf '999999999999999999999999999999 + 1\n' | bc
printf 'sum=0; for (i=0; i<4; i=i+1) sum=sum+i; sum\n' | bc
printf 'obase=16; 255\n' | bc
printf 'sqrt(81); 5 <= 3\n' | bc
bc -l 'pi > 3 && e > 2'
```

## SEE ALSO

expr, awk, seq
