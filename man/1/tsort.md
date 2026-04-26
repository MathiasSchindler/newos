# TSORT

## NAME

tsort - topological sort

## SYNOPSIS

```
tsort [file]
```

## DESCRIPTION

tsort reads pairs of whitespace-separated items from a file (or standard input), where each pair `A B` expresses the dependency "A comes before B", and writes a topological ordering — one item per line.

## CURRENT CAPABILITIES

- topological ordering of a directed acyclic graph expressed as pairs
- reading from a file or standard input
- detection and reporting of cycles

## OPTIONS

tsort accepts no flags.

## LIMITATIONS

- maximum number of nodes is bounded by an internal static buffer (TSORT_MAX_NODES)
- on a cycle, an error is reported but output for non-cyclic nodes may still be produced; the behaviour is not guaranteed to match GNU tsort exactly
- no option to suppress diagnostic output

## EXAMPLES

- `tsort pairs.txt` — topologically sort items in the file
- `echo "lib app\nbase lib" | tsort` — sort a three-node chain
- `make -n | grep "^cc" | tsort` — order compilation steps by dependency

## SEE ALSO

sort, make
