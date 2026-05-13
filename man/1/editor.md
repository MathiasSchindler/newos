# EDITOR

## NAME

editor - interactive terminal text editor

## SYNOPSIS

```
editor [FILE]
```

## DESCRIPTION

`editor` opens a terminal text editing session for a UTF-8 text file. It is intentionally small: the editor keeps a line-oriented buffer, draws directly to an ANSI-style terminal, and uses the project platform layer for terminal and file operations.

When `FILE` is given, `editor` opens that path if it exists or starts a new buffer for that path if it does not. When no file is given, `editor` starts an unnamed buffer and asks for a file name the first time the buffer is saved.

## CURRENT CAPABILITIES

- open an existing text file or start a new buffer
- insert ordinary text, tabs, and new lines
- move with arrow keys, Home, End, PageUp, and PageDown
- move by words or larger steps with modified arrow keys on terminals that report Shift/Ctrl modifiers
- delete with Backspace and Delete
- save with `Ctrl-S`
- quit with `Ctrl-Q`
- confirm before quitting with unsaved changes
- toggle line numbers with `Ctrl-L`
- search forward with `Ctrl-F`
- undo the most recent edit with `Ctrl-Z`
- cut the current line with `Ctrl-K` and paste it with `Ctrl-U`
- apply basic C-family syntax highlighting for `.c`, `.h`, and `.S` files
- prompt for a file name when saving an unnamed buffer
- preserve whether an opened file ended with a final newline unless the buffer is edited

## CONTROLS

- `Ctrl-S` save the current buffer
- `Ctrl-Q` quit, asking for confirmation if the buffer has unsaved changes
- `Ctrl-F` search forward from the cursor, wrapping once through the file
- `Ctrl-L` toggle line numbers at the start of each visible line
- `Ctrl-Z` undo the most recent edit operation
- `Ctrl-K` cut the current line into the editor clipboard
- `Ctrl-U` paste the editor clipboard at the cursor
- `Arrow keys` move by character or line
- `Ctrl-Left`, `Ctrl-Right` move by word
- `Shift-Left`, `Shift-Right` move by eight characters
- `Shift-Up`, `Shift-Down`, `Ctrl-Up`, `Ctrl-Down` move by five lines
- `Shift-Ctrl-Left`, `Shift-Ctrl-Right` move by five words
- `Home`, `End`, `PageUp`, `PageDown` move within the buffer
- `Backspace`, `Delete` remove text around the cursor

## OPTIONS

- `FILE` edit the named file or use it as the save path for a new file
- `--help` print usage information and exit

## LIMITATIONS

- undo stores one previous buffer state, not a multi-level history
- cut and paste use one editor-local clipboard and do not integrate with the system clipboard
- syntax highlighting is intentionally small and currently recognizes only simple C-family tokens
- modified arrow keys depend on the terminal sending standard CSI modifier escape sequences
- spell checking is not implemented
- the editor assumes an ANSI-style terminal and does not use terminfo
- file contents are handled as UTF-8-oriented text, but invalid byte sequences are only displayed defensively rather than repaired
- newly edited buffers normalize saved line endings to newline-separated text

## EXAMPLES

```
editor notes.txt
editor
```

## SEE ALSO

ed, manual, user-interface, runtime, platform