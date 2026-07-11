# COMM

## NAME

comm - compare two sorted files line by line

## SYNOPSIS

```
comm [-123] FILE1 FILE2
```

## DESCRIPTION

`comm` reads two sorted input files and writes three columns: lines unique to FILE1, lines unique to FILE2, and lines present in both files.

## CURRENT CAPABILITIES

- compare two pre-sorted text files line by line
- suppress any of the three output columns with `-1`, `-2`, and `-3`
- read either input from standard input with `-`

## OPTIONS

- `-1` suppress lines unique to FILE1
- `-2` suppress lines unique to FILE2
- `-3` suppress lines common to both files

## LIMITATIONS

- input must already be sorted in bytewise order
- locale collation and order checking are not implemented
- at most one input should be read from standard input in ordinary use

## EXAMPLES

```
comm old.txt new.txt
comm -23 old.txt new.txt
comm -12 old.txt new.txt
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

sort, uniq, diff, join
