# SEQ

## NAME

seq - print a sequence of numbers

## SYNOPSIS

seq [-w] [-s SEP] [-f FORMAT] [FIRST [STEP]] LAST

## DESCRIPTION

seq prints a sequence of numbers from FIRST to LAST, incrementing by STEP. FIRST defaults to 1 and STEP defaults to 1. All values may be decimal fractions.

## CURRENT CAPABILITIES

- integer and fixed-decimal sequences
- custom separator string
- equal-width zero-padded output
- printf-style format strings

## OPTIONS

- `-w` — equalize output width by padding with leading zeros
- `-s SEP` — use SEP as the separator between numbers (default: newline)
- `-f FORMAT` — use printf-style FORMAT to print each number (e.g. `%05.0f`)

## LIMITATIONS

- floating-point range is limited to fixed-decimal representation; scientific notation is not supported
- no support for arbitrary-precision arithmetic

## EXAMPLES

- `seq 5` — print 1 through 5
- `seq 2 10` — print 2 through 10
- `seq 0 0.5 2` — print 0, 0.5, 1.0, 1.5, 2.0
- `seq -w 1 10` — print 01 through 10
- `seq -s , 1 5` — print `1,2,3,4,5`

## SEE ALSO

yes, printf, bc
