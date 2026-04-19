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

Stage 1 is now underway in the repository rather than being only a design goal.

The shared runtime layer already provides UTF-8 decode, encode, validation, simple case folding, Unicode whitespace checks, word-character checks, and display-width helpers.

Visible tool-level improvements already landed include:

- `wc` counting Unicode code points and display width more accurately
- `rev` preserving UTF-8 sequences while reversing text
- `grep` performing Stage 1 Unicode-aware ignore-case matching in both fixed-string and regex search paths
- `man` performing Unicode-aware keyword lookup for manual pages
- `column`, `fold`, and `fmt` using visual width instead of raw byte count for alignment and wrapping
- `cut` selecting character positions by UTF-8 code point in character mode
- `join` matching keys with Unicode-aware ignore-case behavior and safer whitespace splitting
- `awk` treating Unicode whitespace more consistently for default field splitting
- `tr` translating, deleting, and squeezing literal Unicode characters as whole code points instead of raw bytes
- the shell parser recognizing Unicode whitespace separators more consistently, with interactive input no longer restricted to plain ASCII bytes
- `sed` benefiting from the shared Unicode-aware regex/search layer for pattern matching and substitution
- `file` recognizing UTF-8 and UTF-16 text signatures

Important limitations still remain. The current implementation is intentionally compact and does not yet provide full normalization, grapheme-cluster segmentation, locale-specific collation, or exhaustive case-fold coverage.

## GOALS

The full Unicode-support direction for this repository includes:

- UTF-8 validation and decoding
- Unicode case folding for search and matching
- grapheme-cluster awareness for visible-character operations
- terminal display-width handling for combining marks, CJK, and emoji
- normalization-aware comparison where needed
- consistent behavior across tools and hosted/freestanding builds

## STAGE 1

Stage 1 builds the shared foundation layer and is now partially implemented.

This stage now provides:

- UTF-8 decode and encode helpers in the runtime
- validation of incoming byte streams
- code-point iteration helpers
- basic Unicode whitespace and word-character detection
- display-width logic for combining marks and wide characters
- targeted adoption in the most visible text tools

So far, Stage 1 work has reached `wc`, `rev`, `grep`, `man`, `column`, `fold`, `fmt`, `cut`, `join`, `awk`, `tr`, important `sed` paths, and the shell tokenizer/parser.

Remaining Stage 1 work is now mainly about broadening the same behavior across the rest of the text-processing layer and filling in a few remaining edge cases.

## LATER STAGES

After the current Stage 1 rollout is broadened, the next major steps are:

- fuller case-fold coverage beyond the compact Stage 1 tables
- Unicode-aware character classes and semantics across the broader regex and text-processing layer
- broader shell expansion, editing, and quoting polish for Unicode-heavy interactive use
- richer Unicode class and range semantics where current Stage 1 behavior is still intentionally compact
- grapheme-cluster awareness for visibly single characters made of multiple code points
- normalization-aware comparison where exact equivalence matters
- policy decisions around compiler identifiers and source normalization

## DESIGN RULES

- Keep internal text in UTF-8 rather than converting to UTF-16 or wide-character host APIs.
- Do not require a system Unicode library at run time.
- Prefer small generated tables over handwritten per-tool special cases.
- Treat invalid UTF-8 as data that must be handled safely and predictably.

## SEE ALSO

runtime, shell, man, grep, wc, rev
