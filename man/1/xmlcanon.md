# XMLCANON

## NAME

xmlcanon - write a deterministic canonical-ish XML representation

## SYNOPSIS

```
xmlcanon [--strip-comments] [--sort-attrs] [--expand-empty] [FILE ...]
xmlcanon -h
xmlcanon --help
```

## DESCRIPTION

The `xmlcanon` tool rewrites XML into a deterministic representation useful for simple comparisons and normalization workflows.

## CURRENT CAPABILITIES

- rewrite start, end, and empty-element tags in a consistent style
- optionally sort attributes by textual name
- optionally strip comments
- optionally expand empty-element tags into start/end pairs
- preserve most non-tag source text as tokenized

## OPTIONS

- `--strip-comments` omit comments from output
- `--sort-attrs` sort attributes by textual name on each element
- `--expand-empty` write `<empty></empty>` instead of `<empty/>`
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- This is not W3C Canonical XML.
- No canonical hash output mode such as `--hash=sha256` is implemented yet.
- Namespaces, entity spelling, line endings, and attribute values are not normalized to canonical XML rules.
- Source text outside rewritten tags is mostly preserved as tokenized.

## EXAMPLES

```
xmlcanon --sort-attrs document.xml
xmlcanon --strip-comments --expand-empty document.xml
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

xmldiff, xmlfmt, xmlmin