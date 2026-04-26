# MANUAL

## NAME

manual - style guide for project manual pages

## DESCRIPTION

The project manual is written in Markdown and rendered by the repository-local `man` tool. Pages should stay readable as plain Markdown while also producing clean terminal output through `man`.

Manual pages are user-facing documentation, not planning notes. Keep them close to the behavior that exists in the tree, update them alongside code changes, and prefer small, direct wording over exhaustive host-tool compatibility lists.

## FILE LAYOUT

- command pages live in `man/1/<tool>.md`
- project and contributor reference pages live in `man/7/<topic>.md`
- generated output, experiments, and historical notes should not be added under `man/`
- file names should match the topic users pass to `man`

## PAGE STRUCTURE

Use uppercase headings for page titles and section names.

Command pages normally use this order:

```
# TOOL

## NAME

tool - short description

## SYNOPSIS

...

## DESCRIPTION

...

## CURRENT CAPABILITIES

...

## OPTIONS

...

## LIMITATIONS

...

## EXAMPLES

...

## SEE ALSO

...
```

Section 7 pages can omit command-specific sections such as `SYNOPSIS`, `OPTIONS`, or `EXAMPLES` when they do not apply. They should still include `NAME`, `DESCRIPTION`, and `SEE ALSO` when useful.

## SYNOPSIS

Use an untagged fenced code block for syntax forms. Do not use bold Markdown, inline code, or shell-language tags in synopsis blocks.

Keep each synopsis line readable at terminal width. Break long forms at option boundaries and indent continuation lines by two spaces:

```
tool [-a] [-b VALUE] [-c VALUE]
  [--long-option VALUE] FILE ...
```

Use square brackets for optional arguments, braces only when alternatives need to be grouped, and uppercase placeholder names for operands such as `FILE`, `PATH`, `COMMAND`, or `VALUE`.

## OPTIONS

Prefer a bullet list for short option descriptions:

- `-a` describe the option in sentence style
- `--long[=WHEN]` describe optional option values directly
- `FILE` describe positional operands when that is clearer than prose

Use a Markdown table only when the page already has several closely related flags and the table improves scanning. Avoid mixing table and bullet styles in the same section unless there is a clear reason.

## EXAMPLES

Use an untagged fenced code block when examples are plain commands:

```
tool input.txt
tool -v input.txt output.txt
```

Use bullets only when each example needs a short explanation next to it:

- `tool input.txt` process one file
- `tool -v input.txt` show verbose progress

Do not add `sh`, `text`, or other language tags to example fences. The manual renderer treats these as literal terminal examples, not syntax-highlighted programs.

## LIMITATIONS

The `LIMITATIONS` section should describe known boundaries of the current implementation. It is not a bug tracker or a roadmap, but it is often a useful map of friction points that future development may use for inspiration.

Prefer one bullet per limitation. Name the practical constraint or missing behavior clearly enough that a future contributor can understand the user pain without reading the implementation first. Keep each prose bullet on one source line so the renderer can choose terminal wrapping consistently:

- no recursive mode is implemented yet
- file names are interpreted as bytes rather than locale-aware text

Use nested bullets only for real sublists, such as a fixed protocol profile or a grouped set of supported formats.

## FORMATTING

- use plain Markdown headings, bullets, fenced code blocks, and tables
- use untagged fenced blocks for `SYNOPSIS` and command-only `EXAMPLES`
- keep each prose paragraph or bullet as one logical source line, and let the renderer wrap for the current output width
- use backticks for command names, flags, file names, variables, and literal values
- avoid changing content while doing formatting-only cleanup
- keep wording direct and current-tense

Avoid display-only source wrapping. The `man` tool, pager, terminal, or other renderer has better information about screen width and should decide where prose wraps.

## SEE ALSO

man, project-layout, output-style, testing