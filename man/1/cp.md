# CP

## NAME

cp - copy files and directory trees

## SYNOPSIS

cp [OPTIONS] SOURCE DEST
cp [OPTIONS] SOURCE ... DIRECTORY

## DESCRIPTION

The cp tool copies regular files and, when requested, directory trees. In this project it is intended to provide a practical, dependency-free implementation of common copy behavior.

## CURRENT CAPABILITIES

- copy one file to another
- copy multiple files into a directory
- recursive copy
- preserve selected metadata
- archive-style copying for common cases

## EXAMPLES

- cp notes.txt notes.bak
- cp -r src backup
- cp -a tree dest

## SEE ALSO

man, mv, ln, rm
