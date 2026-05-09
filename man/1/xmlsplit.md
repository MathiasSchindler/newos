# XMLSPLIT

## NAME

xmlsplit - split selected XML subtrees into separate files

## SYNOPSIS

```
xmlsplit [--force] [--max N] SELECTOR PREFIX [FILE]
xmlsplit -h
xmlsplit --help
```

## DESCRIPTION

The `xmlsplit` tool writes each selected element subtree to a numbered output file using PREFIX and a six-digit sequence number.

## CURRENT CAPABILITIES

- select element subtrees with the project selector syntax
- support final-component attribute and same-name sibling position predicates
- write one file per selected subtree
- name files such as `PREFIX000001.xml`, `PREFIX000002.xml`, and so on
- refuse to overwrite existing output files unless `--force` is used
- stop after N output files with `--max N`
- read from one file or standard input

## OPTIONS

- `--force` allow overwriting generated output file names
- `--max N` write at most N selected subtrees
- `SELECTOR` select element subtrees using the project selector syntax
- `PREFIX` prefix for generated output file names
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Output directories are not created automatically yet; create them before running `xmlsplit`.
- Naming by key value, such as `--by-key @id`, is not implemented yet.
- Extracted subtrees do not include ancestor namespace declarations unless those declarations are inside the selected subtree.

## EXAMPLES

```
xmlsplit //entry entries/entry- feed.xml
xmlsplit --max 10 //article articles/article- dump.xml
```

## SEE ALSO

xmlcut, xmlhead, xmltail