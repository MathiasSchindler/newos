# MAN

## NAME

man - view project-local manual pages stored in the repository

## SYNOPSIS

man [SECTION] TOPIC
man -k KEYWORD
man -l FILE

## DESCRIPTION

The project man tool reads markdown manual pages from the repository-local man directory. It does not depend on the host system manual database and does not install files into system locations.

Pages are searched in sections such as 1, 5, and 7.

## OPTIONS

- SECTION selects a specific manual section
- -k searches page names and page content for a keyword
- -l displays a specific markdown file directly

## EXAMPLES

- man ls
- man 1 cp
- man -k compiler
- man -l man/1/ncc.md

## SEE ALSO

ls, cp, ncc
