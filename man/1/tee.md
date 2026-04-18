# TEE

## NAME

tee - duplicate standard input to files and standard output

## SYNOPSIS

```
tee [-a] [-i] [file ...]
```

## DESCRIPTION

`tee` reads from standard input and writes to standard output and to each FILE
simultaneously, allowing a pipeline to be observed at an intermediate stage.

## CURRENT CAPABILITIES

- Write stdin to stdout and to one or more files concurrently
- Append to files instead of truncating with `-a`
- Ignore SIGINT with `-i`

## OPTIONS

- `-a` — append to each FILE rather than overwriting
- `-i` — ignore the SIGINT signal

## LIMITATIONS

- No `-p` (ignore write errors to files while still writing to stdout).

## EXAMPLES

```
make 2>&1 | tee build.log
echo "test" | tee file1.txt file2.txt
cat bigfile | tee -a log.txt | wc -l
```

## SEE ALSO

cat, dd
