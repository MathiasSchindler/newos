# UNICODE

## NAME

unicode - design notes and implementation roadmap for full Unicode support

## DESCRIPTION

This project aims to support UTF-8 text safely across the tool set and to grow toward fuller Unicode-aware behavior without adding large external runtime dependencies. It should also provide a clear Überblick of how text is interpreted across tools.

The intended long-term model is:

- UTF-8 as the default text encoding for source and tool input/output
- compact in-tree Unicode tables generated ahead of time and checked into the repository
- consistent decoding, validation, display-width handling, and case folding across tools
- graceful handling of invalid byte sequences instead of crashes or silent corruption

## CURRENT STATE

At present, most tools are byte-oriented and therefore preserve UTF-8 input reasonably well, but much of the parsing and matching logic remains ASCII-centric.

Examples of partial Unicode awareness already present in the tree include:

- `wc` counting non-continuation UTF-8 bytes as characters
- `rev` avoiding naive reversal inside UTF-8 sequences
- `file` recognizing UTF-8 and UTF-16 text signatures

Important limitations still exist for case-insensitive matching, word boundaries, field splitting, and terminal display width.

## GOALS

The full Unicode-support direction for this repository includes:

- UTF-8 validation and decoding
- Unicode case folding for search and matching
- grapheme-cluster awareness for visible-character operations
- terminal display-width handling for combining marks, CJK, and emoji
- normalization-aware comparison where needed
- consistent behavior across tools and hosted/freestanding builds

## STAGE 1

Stage 1 builds the shared foundation layer.

This stage should provide:

- UTF-8 decode and encode helpers in the runtime
- validation of incoming byte streams
- code-point iteration helpers
- basic Unicode whitespace detection
- display-width logic for combining marks and wide characters
- targeted adoption in the most visible text tools

The first tools expected to benefit from Stage 1 are `wc`, `rev`, `grep`, and `man`.

## LATER STAGES

After the runtime layer is in place, later work can extend Unicode handling to:

- case-insensitive search in `grep` and manual-page lookup
- proper alignment in `column`, `fmt`, and `fold`
- safer character-oriented behavior in `cut`, `join`, `awk`, and the shell
- policy decisions around compiler identifiers and source normalization

## DESIGN RULES

- Keep internal text in UTF-8 rather than converting to UTF-16 or wide-character host APIs.
- Do not require a system Unicode library at run time.
- Prefer small generated tables over handwritten per-tool special cases.
- Treat invalid UTF-8 as data that must be handled safely and predictably.

## SEE ALSO

runtime, shell, man, grep, wc, rev
