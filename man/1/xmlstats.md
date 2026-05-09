# XMLSTATS

## NAME

xmlstats - report XML statistics and name histograms

## SYNOPSIS

```
xmlstats [FILE ...]
xmlstats -h
xmlstats --help
```

## DESCRIPTION

The `xmlstats` tool reports document statistics and first-seen name histograms for element and attribute names.

## CURRENT CAPABILITIES

- count elements, attributes, non-blank text nodes, text bytes, comments, CDATA, processing instructions, and DOCTYPE declarations
- report maximum element depth
- print element-name and attribute-name histograms
- grow name tables as new names are encountered

## OPTIONS

- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Name histograms are kept in in-process memory.
- Per-element attribute frequency, depth distribution, and per-element text-length distribution are not reported yet.
- No `--json`, `--tsv`, or `--top N` output mode is implemented yet.
- Namespace prefixes are counted textually as part of the name.
- Text byte counts are source-byte counts, not decoded character counts.

## EXAMPLES

```
xmlstats document.xml
xmlstats *.xml
```

## SEE ALSO

xmlcount, xmltokens, xmlcheck