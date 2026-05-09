# XMLSAFE

## NAME

xmlsafe - check XML against conservative safety rules

## SYNOPSIS

```
xmlsafe [--allow-doctype] [--allow-pi] [--allow-comments]
  [--max-depth N] [--max-text N] [FILE ...]
xmlsafe -h
xmlsafe --help
```

## DESCRIPTION

The `xmlsafe` tool checks XML against conservative structural safety rules before passing input to stricter downstream systems.

## CURRENT CAPABILITIES

- reject DOCTYPE declarations, processing instructions, and comments by default
- allow selected metadata with explicit `--allow-*` options
- enforce maximum element depth and maximum text or CDATA token size
- report line and column diagnostics for unsafe constructs
- still perform normal well-formedness checks

## OPTIONS

- `--allow-doctype` allow DOCTYPE declarations
- `--allow-pi` allow processing instructions
- `--allow-comments` allow comments
- `--max-depth N` reject documents deeper than N elements
- `--max-text N` reject text or CDATA tokens longer than N source bytes
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- This is a conservative structural checker, not a complete XML security sandbox.
- External resources are not loaded, and schemas are not validated.
- Token-size checks happen after the document-buffered parser has read the complete input.

## EXAMPLES

```
xmlsafe input.xml
xmlsafe --allow-comments input.xml
xmlsafe --max-depth 32 --max-text 65536 input.xml
```

## SEE ALSO

xmlcheck, xmlvalidate, xmlnscheck