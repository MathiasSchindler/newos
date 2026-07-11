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
- smart-case matching with `-S` or `--smart-case`
- whole-word matching with `-w` or `--word-regexp`
- inverted matching with `-v` or `--invert-match`
- line-number output by default, with `--no-line-number` to suppress it
- optional column-number output with `--column`
- file-name prefixes when searching more than one path or recursively
- heading output with `--heading`
- count-only output with `-c` or `--count`
- only-matching output with `-o` or `--only-matching`
- quiet exit-code-only mode with `-q` or `--quiet`
- list files with matches using `-l` or `--files-with-matches`
- list files without matches using `-L` or `--files-without-match`
- include or exclude paths with `-g GLOB` or `--glob GLOB`; a leading `!` negates the glob
- include or exclude aliases with `--include GLOB` and `--exclude GLOB`
- simple type filters with `-t TYPE`, `--type TYPE`, and `--type-not TYPE`
- built-in type listing with `--type-list`
- `.gitignore`, `.ignore`, `.rgignore`, and explicit `--ignore-file FILE` support
- deterministic path traversal with `--sort path`
- bounded traversal with `--max-depth N`
- per-file match limits with `-m N` or `--max-count N`
- candidate-file listing with `--files`
- summary counters with `--stats`
- optional colored match highlighting with `--color=WHEN`

## OPTIONS

- `-i`, `--ignore-case` search case-insensitively
- `-s`, `--case-sensitive` search case-sensitively
- `-S`, `--smart-case` search case-insensitively unless a pattern contains uppercase ASCII
- `-F`, `--fixed-strings` treat PATTERN as a literal string
- `-e PATTERN`, `--regexp PATTERN` add an explicit search pattern; may be repeated
- `-w`, `--word-regexp` require word boundaries around each match
- `-v`, `--invert-match` select non-matching lines
- `-n`, `--line-number` show line numbers
- `--no-line-number` suppress line numbers
- `--column` show the first match column for each output line or match
- `-c`, `--count` print the number of matching lines per file
- `-o`, `--only-matching` print only matching text spans
- `-m N`, `--max-count N` stop after N matching lines per file
- `-q`, `--quiet` suppress output and report only the exit status
- `-l`, `--files-with-matches` print each matching file path once
- `-L`, `--files-without-match` print each non-matching file path once
- `-H`, `--with-filename` always print file-name prefixes
- `--no-filename` suppress file-name prefixes
- `--heading` print each matching path as a heading before its matches
- `--no-heading` suppress heading mode
- `--no-messages` suppress open/read diagnostics while preserving exit status
- `--hidden` include hidden files and directories
- `--ignore` honor ignore files; this is the default
- `--no-ignore` do not apply ignore files
- `--ignore-file FILE` read additional ignore patterns from FILE
- `-g GLOB`, `--glob GLOB` include paths matching GLOB; use `!GLOB` to exclude paths
- `--include GLOB` include paths matching GLOB
- `--exclude GLOB` exclude paths matching GLOB
- `-t TYPE`, `--type TYPE` restrict search to a file type or extension
- `--type-not TYPE` exclude a file type or extension
- `--type-list` list built-in file types
- `--follow` use follow-aware path classification where the platform backend exposes it
- `--max-depth N` limit recursive descent depth
- `--sort path` sort directory entries by path before traversal
- `--stats` print summary counters after searching
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
- Ignore-file parsing is intentionally small: blank lines, comments, negation with `!`, directory-only patterns ending in `/`, slash-relative patterns, and basename globs are supported; advanced gitignore escaping and precedence edge cases are not complete.
- No PCRE2 support, multiline search, decompression, JSON output, or parallel directory walking is implemented.
- Patterns are limited to 64 KiB, nesting is limited to 64 groups, and each record search has a bounded matcher work budget.
- `--follow` uses the available platform follow-aware path information, but full symlink-loop detection is not implemented.
- Binary files are skipped when a NUL byte is encountered.
- Directory traversal uses fixed-size in-process entry buffers, so extremely large directories may need future streaming iteration support.

## EXAMPLES

```
rg TODO src
rg -n "main" src/tools
rg -i -t c "platform_" src
rg --files -g "*.md" man
rg --hidden -g "!build/*" needle .
rg -S -e TODO -e FIXME --sort path src
rg --include "*.c" --exclude "build/*" -o --column platform_ .
rg --no-ignore --stats needle .
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

grep, find, man, output-style