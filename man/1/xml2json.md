# XML2JSON

## NAME

xml2json - convert XML to a simple structural JSON form

## SYNOPSIS

```
xml2json [FILE ...]
xml2json -h
xml2json --help
```

## DESCRIPTION

The `xml2json` tool converts XML into a compact structural JSON representation containing element names, attributes, and ordered child arrays.

## CURRENT CAPABILITIES

- emit element names and attributes
- emit ordered child arrays
- emit non-blank text and CDATA as text child objects
- produce deterministic compact JSON

## OPTIONS

- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Comments, processing instructions, and DOCTYPE declarations are not represented.
- Repeated child names remain repeated child objects rather than being grouped.
- There is no alternate XML-to-JSON convention mode, such as merged same-name children or BadgerFish-style output.
- Entity references are not decoded before JSON output.

## EXAMPLES

```
xml2json document.xml
xml2json feed.xml > feed.json
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

xml2yaml, xml2csv, xml2lines, xmltokens