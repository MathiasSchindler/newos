# SEQ

## NAME

seq - print a sequence of numbers

## SYNOPSIS

```
seq [-w] [-s SEP] [-f FORMAT] [FIRST [STEP]] LAST
```

## DESCRIPTION

seq prints a sequence of numbers from FIRST to LAST, incrementing by STEP. FIRST defaults to 1 and STEP defaults to 1. All values may be decimal fractions.

## CURRENT CAPABILITIES

- integer and fixed-decimal sequences
- scientific notation input
- large decimal arithmetic backed by the project bignum implementation
- custom separator string
- equal-width zero-padded output
- printf-style `f`, `e`, `E`, `g`, and `G` format strings with common flags,
  width, precision, signs, zero padding, left adjustment, and grouped fixed output

## OPTIONS

- `-w` — equalize output width by padding with leading zeros
- `-s SEP` — use SEP as the separator between numbers (default: newline)
- `-f FORMAT` — use printf-style FORMAT to print each number (e.g. `%+08.2f`, `%.3e`, `%'f`)

## LIMITATIONS

- Grouping uses comma separators; locale-specific decimal separators and
  thousands grouping are not loaded from the host locale.

## EXAMPLES

- `seq 5` — print 1 through 5
- `seq 2 10` — print 2 through 10
- `seq 0 0.5 2` — print 0, 0.5, 1.0, 1.5, 2.0
- `seq 1e3 2e3 5e3` — print 1000, 3000, 5000
- `seq -w 1 10` — print 01 through 10
- `seq -s , 1 5` — print `1,2,3,4,5`
- `seq -f "%'f" 1000 1000 3000` — print grouped fixed-width numbers

## SEE ALSO

yes, printf, bc
