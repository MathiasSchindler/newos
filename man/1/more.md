# MORE

## NAME

more - page through text one screen at a time (forward only)

## SYNOPSIS

```
more [-N] [-p PATTERN] [--color[=WHEN]] [+/PATTERN] [file ...]
```

## DESCRIPTION

more displays text files (or standard input) one screen at a time, pausing
after each page for the user to press space to continue or `q` to quit. It
keeps a forward-oriented interface but now adds a few lightweight search and
navigation conveniences.

## CURRENT CAPABILITIES

- forward paging through one or more files
- optional line numbering with `-N`
- initial search-jump with `-p PATTERN` or `+/PATTERN`
- simple interactive stepping with `Space`, `Enter`, `g`, `G`, `n`, and `q`
- reading from standard input when no files are given
- per-file paging when multiple files are supplied
- shared prompt styling with `--color=WHEN`

## OPTIONS

- `-N` — prefix each line with its line number
- `-p PATTERN` or `+/PATTERN` — start near the first matching line
- `--color[=WHEN]` — control prompt styling with `auto`, `always`, or `never`

## LIMITATIONS

- no backward navigation (use less for that)
- the search/navigation feature set is intentionally small compared with full GNU/BSD pagers
- no compatibility implementation for the wider collection of GNU/BSD `more` flags

## EXAMPLES

- `more file.txt` — page through a file
- `more +/TODO notes.txt` — jump to the first matching line
- `ls -l | more` — page through command output

## SEE ALSO

less, man, cat
