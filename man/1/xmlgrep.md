# XMLGREP

## NAME

xmlgrep - search XML text and attribute values

## SYNOPSIS

```
xmlgrep [-iFvclLoq] PATTERN [FILE ...]
xmlgrep [--ignore-case] [--fixed-strings] [--invert-match]
	[--count] [--files-with-matches] [--files-without-match]
	[--only-matching] [--quiet] PATTERN [FILE ...]
xmlgrep -h
xmlgrep --help
```

## DESCRIPTION

The `xmlgrep` tool searches text nodes and attribute values with the project regex engine. Matching output includes the XML path and the matching value.

Grep-style switches operate on XML value records. A value record is either a non-blank text or CDATA node, or one attribute value. Element names are not searched.

## CURRENT CAPABILITIES

- search non-blank text nodes and attribute values
- report XML paths for matches
- include the file name when a file path is provided
- perform case-insensitive matching with `-i` or `--ignore-case`
- treat patterns as fixed strings with `-F` or `--fixed-strings`
- invert selected value records with `-v` or `--invert-match`
- count selected value records with `-c` or `--count`
- list files with or without selected matches using `-l` and `-L`
- print only the matching slice of selected values with `-o`
- suppress normal output with `-q` or `--quiet`
- accept clustered short options such as `-iv` or `-Fo`

## OPTIONS

- `-i` match without ASCII case sensitivity
- `--ignore-case` match without ASCII case sensitivity
- `-F`, `--fixed-strings` treat PATTERN as literal text instead of regex syntax
- `-v`, `--invert-match` select value records that do not match PATTERN
- `-c`, `--count` print the number of selected value records
- `-l`, `--files-with-matches` print file names with at least one selected value record
- `-L`, `--files-without-match` print file names with no selected value records
- `-o`, `--only-matching` print only matched slices instead of complete values; ignored with `-v`
- `-q`, `--quiet`, `--silent` suppress normal output and use only the exit status
- `PATTERN` search pattern in the toolkit regex syntax
- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## EXIT STATUS

- `0` at least one value record or file was selected
- `1` no value record or file was selected
- `2` an input, parse, or usage error occurred

## LIMITATIONS

- Element names are not searched.
- There is no name-search mode yet; matches are limited to text and attribute values.
- Recursive directory traversal and include/exclude glob filtering are not implemented yet.
- Multiple `-e` patterns and pattern files are not implemented yet.
- Regex syntax is the toolkit lightweight regex syntax, not PCRE.
- Search values are copied into temporary NUL-terminated storage before matching.

## EXAMPLES

```
xmlgrep error log.xml
xmlgrep -i TODO document.xml
xmlgrep -Fo '<tag>' document.xml
xmlgrep -c warning feed.xml
xmlgrep -L deprecated *.xml
xmlgrep 'hello' file.xml
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

xmlget, xml2lines, grep