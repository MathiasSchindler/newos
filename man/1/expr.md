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

- `ARG1 + ARG2` — arbitrary-precision integer addition
- `ARG1 - ARG2` — arbitrary-precision integer subtraction
- `ARG1 * ARG2` — arbitrary-precision integer multiplication
- `ARG1 / ARG2` — arbitrary-precision integer division (truncates toward zero)
- `ARG1 % ARG2` — arbitrary-precision integer remainder

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

## IMPLEMENTATION NOTES

`expr` uses the shared signed big-number primitives from `src/shared/bignum.{c,h}`
to provide arbitrary-precision integer arithmetic. The implementation supports
integers up to `BN_MAX_DECIMAL_DIGITS`; with the default `BN_MAX_DIGITS=8192`
and base-1000000000 storage, that is approximately 73728 decimal digits. This
is well beyond the limits of native integer types while still using fixed-size,
freestanding storage.

Arithmetic operations (`+`, `-`, `*`, `/`, `%`) and numeric comparisons (`<`,
`<=`, `>`, `>=`) all benefit from this arbitrary-precision support. String
operations and logical operators work as documented without numeric limits.

## LIMITATIONS

- No `match` or `:` (regex match) operator.
- No floating-point arithmetic.
- Numeric values exceeding the configured bignum capacity will trigger an overflow error.
- Each operator and operand must be a separate shell argument.

## EXAMPLES

Basic usage:

```
expr 2 + 3
expr 10 / 3
expr length "hello"
expr substr "hello world" 7 5
expr "$x" \> 0
```

Arbitrary-precision arithmetic:

```
expr 99999999999999999999 + 1
# outputs: 100000000000000000000

expr 12345678901234567890 \* 98765432109876543210
# outputs: 1219326311370217952237463801111263526900
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

test, awk, bc
