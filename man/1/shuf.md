# SHUF

## NAME

shuf - shuffle lines or generate random permutations

## SYNOPSIS

shuf [-n COUNT] [-r] [-z] [-o FILE] [--random-source=FILE] [file]
shuf -e [-n COUNT] [-r] [-z] [-o FILE] [--random-source=FILE] arg ...
shuf -i LO-HI [-n COUNT] [-r] [-o FILE] [--random-source=FILE]

## DESCRIPTION

shuf randomly permutes its input lines and writes the result to standard output. With `-e` it treats its command-line arguments as lines; with `-i` it generates numbers in a range.

## CURRENT CAPABILITIES

- shuffling lines from a file or standard input
- shuffling command-line arguments as lines (`-e`)
- generating a shuffled range of integers (`-i LO-HI`)
- limiting output to at most N items (`-n`)
- sampling with replacement (`-r`)
- NUL-delimited output (`-z`)
- writing output to a file (`-o`)
- seeding from an external random source (`--random-source`)

## OPTIONS

- `-n COUNT` — output at most COUNT lines
- `-r` — allow repetition (sample with replacement)
- `-z` / `--zero-terminated` — use NUL as the line terminator
- `-o FILE` / `--output FILE` — write output to FILE instead of standard output
- `--random-source=FILE` — read random bytes from FILE instead of the default source
- `-e` — treat remaining arguments as input lines
- `-i LO-HI` — generate integers between LO and HI inclusive (no file argument)

## LIMITATIONS

- maximum input size is bounded by an internal static buffer (SHUF_MAX_ITEMS)
- no support for reading from multiple files

## EXAMPLES

- `shuf file.txt` — randomly permute lines
- `shuf -n 3 file.txt` — pick 3 random lines
- `shuf -i 1-100 -n 5` — pick 5 random numbers between 1 and 100
- `shuf -e alpha beta gamma` — shuffle three words

## SEE ALSO

sort, awk, seq
