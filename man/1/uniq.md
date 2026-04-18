# UNIQ

## NAME

uniq - report or filter adjacent duplicate lines

## SYNOPSIS

uniq [-cduiz] [-f FIELDS] [-s CHARS] [-w CHARS] [file]

## DESCRIPTION

uniq reads lines from a file or standard input and filters out or annotates adjacent duplicate lines. Input must be sorted for duplicates to be detected across the whole file.

## CURRENT CAPABILITIES

- filter duplicate adjacent lines
- count occurrences with a prefix
- print only duplicate lines or only unique lines
- case-insensitive comparison
- field- and character-skipping before comparison
- limit comparison to a fixed number of characters
- zero-terminated record mode

## OPTIONS

- `-c` — prefix each output line with its occurrence count
- `-d` — print only lines that appear more than once
- `-u` — print only lines that appear exactly once
- `-i` — ignore case when comparing lines
- `-z` — use NUL as the line delimiter instead of newline
- `-f FIELDS` — skip the first FIELDS whitespace-separated fields before comparing
- `-s CHARS` — skip the first CHARS characters before comparing
- `-w CHARS` — compare at most CHARS characters per line

## LIMITATIONS

- operates on adjacent lines only; sorting is the caller's responsibility
- no support for `-D` (print all duplicate lines) found in some GNU versions

## EXAMPLES

- `uniq file.txt` — remove adjacent duplicates
- `sort file.txt | uniq -c` — count distinct lines
- `uniq -d file.txt` — print only duplicated lines
- `uniq -u file.txt` — print only unique lines
- `uniq -f 2 file.txt` — skip 2 fields before comparing

## SEE ALSO

sort, comm, wc
