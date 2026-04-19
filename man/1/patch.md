# PATCH

## NAME

patch - apply a unified diff patch to files

## SYNOPSIS

patch [-pN] [-R] [-b] [-o outfile] [-i patchfile] [patchfile]

## DESCRIPTION

patch reads a unified-format diff and applies it to the corresponding source files. The patch may be given via `-i`, as a positional argument, or on standard input.

## CURRENT CAPABILITIES

- applying unified diff hunks to existing files
- reading patches from standard input, `-i`, or a positional patch file
- stripping leading path components from filenames in the patch
- reverse-applying a patch
- creating a `.orig` backup before modifying a file
- writing patched output to a separate file with `-o`

## OPTIONS

- `-pN` — strip N leading path components from filenames in the diff header (e.g. `-p1` strips the leading `a/` or `b/` prefix)
- `-R` — apply the patch in reverse
- `-b` — create a `.orig` backup of each file before patching
- `-o outfile` — write patched content to outfile instead of modifying the source file
- `-i patchfile` — read the patch from patchfile instead of standard input

## LIMITATIONS

- only unified diff format (`--- / +++` headers, `@@ ... @@` hunks) is supported; context and ed-script formats are not
- binary patches are not supported
- `--dry-run` is not implemented
- hunk matching is conservative; patches generated against substantially changed
  files may fail instead of applying with large fuzz

## EXAMPLES

- `patch -p1 < changes.patch` — apply a patch, stripping one path component
- `patch -i fix.patch -b` — apply with backup
- `patch -R -i fix.patch` — reverse a previously applied patch

## SEE ALSO

diff, cp, ed
