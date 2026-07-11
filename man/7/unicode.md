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

Stage 2 is implemented as a compact extension of the shared UTF-8 runtime. It remains dependency-free and does not load a system locale or full collation database.

The shared runtime provides UTF-8 decode, encode, validation, simple case folding, Unicode whitespace and word-break classes, grapheme-cluster iteration, normalization-aware comparison, configurable display width, and focused transcoding.

Visible tool-level improvements already landed include:

- `wc` counting Unicode code points while measuring grapheme-aware line width
- `rev` reversing shared grapheme clusters instead of private UTF-8 groups
- `grep` performing Stage 1 Unicode-aware ignore-case matching in both fixed-string and regex search paths
- `man` performing Unicode-aware keyword lookup plus display-width-aware wrapping and table alignment
- `column`, `fold`, `fmt`, `expand`, and `unexpand` using visual width instead of raw byte count for alignment, wrapping, and tab stops
- `cut` selecting extended grapheme clusters in character mode
- `join` matching keys with Unicode-aware ignore-case behavior and safer whitespace splitting
- `awk` treating Unicode whitespace more consistently for default field splitting
- `tr` translating, deleting, and squeezing literal Unicode characters as whole code points instead of raw bytes
- the shell parser recognizing Unicode whitespace separators more consistently, with interactive input no longer restricted to plain ASCII bytes
- `sed` benefiting from the shared Unicode-aware regex/search layer for pattern matching and substitution
- `sort --normalize` comparing canonically equivalent keys and lines
- the XML tool family transcoding UTF-16 BOM input and declaration-selected ISO-8859-1 or Windows-1252 input to internal UTF-8
- `file` recognizing UTF-8 and UTF-16 text signatures

`NEWOS_AMBIGUOUS_WIDTH=2` selects double-column treatment for assigned East Asian Ambiguous characters in shared tool and TUI width paths. The default is width 1.

Important limitations remain. Canonical decomposition focuses on Latin-1 plus algorithmic Hangul and common combining classes. Grapheme and word segmentation implement the most useful extended rules without shipping the complete Unicode property database. Locale-specific collation and exhaustive case folding are intentionally out of scope.

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
- display-width logic for combining marks, ANSI escapes, tabs, and wide East Asian/emoji characters
- targeted adoption in the most visible text tools

So far, Stage 1 work has reached `wc`, `rev`, `grep`, `man`, `column`, `expand`, `unexpand`, `fold`, `fmt`, `cut`, `join`, `awk`, `tr`, important `sed` paths, and the shell tokenizer/parser.

Remaining Stage 1 work is now mainly about broadening the same behavior across the rest of the text-processing layer and filling in a few remaining edge cases.

## STAGE 2

Stage 2 adds:

- forward, backward, and streaming grapheme-cluster iteration
- canonical-equivalence comparison and substring search
- caller-selected East Asian Ambiguous width
- UTF-16 LE/BE codecs with BOM and surrogate support
- ISO-8859-1 and Windows-1252 transcoders
- reusable word-break classification, boundary testing, and span iteration

The internal representation remains UTF-8. Transcoding occurs only at explicit input/output boundaries.

## DESIGN RULES

- Keep internal text in UTF-8 rather than converting to UTF-16 or wide-character host APIs.
- Do not require a system Unicode library at run time.
- Prefer small generated tables over handwritten per-tool special cases.
- Treat invalid UTF-8 as data that must be handled safely and predictably.

## SEE ALSO

runtime, shell, man, grep, wc, rev
