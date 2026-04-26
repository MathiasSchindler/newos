# RIPGREP

## NAME

ripgrep - recursively search files for matching text

## SYNOPSIS

```
ripgrep [OPTIONS] PATTERN [PATH ...]
ripgrep --files [OPTIONS] [PATH ...]
```

## DESCRIPTION

`ripgrep` searches files under each PATH, defaulting to the current directory. It is a project-native subset inspired by `rg`, using the shared regular-expression, terminal-color, file, and platform layers rather than shelling out to a host tool.

Hidden files and directories are skipped by default. Use `--hidden` when dotfiles or dot-directories should be included.

## CURRENT CAPABILITIES

- recursive directory search by default
- regular-expression search through the shared regex engine
- fixed-string matching with `-F` or `--fixed-strings`
- case-insensitive matching with `-i` or `--ignore-case`
- whole-word matching with `-w` or `--word-regexp`
- inverted matching with `-v` or `--invert-match`
- line-number output by default, with `--no-line-number` to suppress it
- file-name prefixes when searching more than one path or recursively
- count-only output with `-c` or `--count`
- quiet exit-code-only mode with `-q` or `--quiet`
- list files with matches using `-l` or `--files-with-matches`
- list files without matches using `-L` or `--files-without-match`
- include or exclude paths with `-g GLOB` or `--glob GLOB`; a leading `!` negates the glob
- simple type filters with `-t TYPE` or `--type TYPE`
- candidate-file listing with `--files`
- optional colored match highlighting with `--color=WHEN`

## OPTIONS

- `-i`, `--ignore-case` search case-insensitively
- `-F`, `--fixed-strings` treat PATTERN as a literal string
- `-w`, `--word-regexp` require word boundaries around each match
- `-v`, `--invert-match` select non-matching lines
- `-n`, `--line-number` show line numbers
- `--no-line-number` suppress line numbers
- `-c`, `--count` print the number of matching lines per file
- `-q`, `--quiet` suppress output and report only the exit status
- `-l`, `--files-with-matches` print each matching file path once
- `-L`, `--files-without-match` print each non-matching file path once
- `-H`, `--with-filename` always print file-name prefixes
- `--no-filename` suppress file-name prefixes
- `--hidden` include hidden files and directories
- `-g GLOB`, `--glob GLOB` include paths matching GLOB; use `!GLOB` to exclude paths
- `-t TYPE`, `--type TYPE` restrict search to a file type or extension
- `--files` print candidate files instead of searching contents
- `--color[=WHEN]` highlight matches and prefixes using `auto`, `always`, or `never`

## FILE TYPES

The built-in type names are intentionally small:

- `c` matches `.c` and `.h`
- `cpp` or `c++` matches common C++ source and header extensions
- `md` or `markdown` matches Markdown files
- `sh` or `shell` matches shell scripts
- `txt` or `text` matches text and Markdown-like plain-text files

An unknown TYPE is treated as a literal file extension, so `-t py` matches `.py` files.

## LIMITATIONS

- This is a practical source-search subset, not a complete clone of upstream ripgrep.
- No `.gitignore`, `.ignore`, or `.rgignore` parsing is implemented yet, so glob filters are the current way to tune traversal.
- No PCRE2 support, multiline search, decompression, JSON output, or parallel directory walking is implemented.
- Binary files are skipped when a NUL byte is encountered.
- Directory traversal uses fixed-size in-process entry buffers, so extremely large directories may need future streaming iteration support.

## EXAMPLES

```
rg TODO src
rg -n "main" src/tools
rg -i -t c "platform_" src
rg --files -g "*.md" man
rg --hidden -g "!build/*" needle .
```

## SEE ALSO

grep, find, man, output-style