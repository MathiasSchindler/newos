# EXPR

## NAME

expr - evaluate an expression

## SYNOPSIS

```
expr EXPRESSION
```

## DESCRIPTION

`expr` evaluates EXPRESSION, prints the result to standard output, and exits
0 if the result is non-zero and non-empty, or 1 otherwise. Each token of the
expression must be a separate argument.

## CURRENT CAPABILITIES

**Arithmetic:**

- `ARG1 + ARG2` — integer addition
- `ARG1 - ARG2` — integer subtraction
- `ARG1 * ARG2` — integer multiplication
- `ARG1 / ARG2` — integer division (truncates toward zero)
- `ARG1 % ARG2` — integer remainder

**Comparison (integers and strings):**

- `ARG1 = ARG2` — equal
- `ARG1 != ARG2` — not equal
- `ARG1 < ARG2`, `ARG1 <= ARG2` — less than / less than or equal
- `ARG1 > ARG2`, `ARG1 >= ARG2` — greater than / greater than or equal

**Logical:**

- `ARG1 | ARG2` — ARG1 if non-zero/non-empty, else ARG2
- `ARG1 & ARG2` — ARG1 if both non-zero/non-empty, else 0

**String functions:**

- `length STRING` — character count of STRING
- `index STRING CHARS` — position of first character from CHARS in STRING
  (1-based; 0 if not found)
- `substr STRING POS LEN` — substring of STRING starting at POS (1-based)
  with length LEN

**Grouping:**

- `( EXPRESSION )` — evaluate EXPRESSION first

## OPTIONS

None.

## LIMITATIONS

- No `match` or `:` (regex match) operator.
- No floating-point arithmetic.
- Integer overflow behaviour is platform-defined.
- Each operator and operand must be a separate shell argument.

## EXAMPLES

```
expr 2 + 3
expr 10 / 3
expr length "hello"
expr substr "hello world" 7 5
expr "$x" \> 0
```

## SEE ALSO

test, awk, bc
