# XML2YAML

## NAME

xml2yaml - convert XML to a simple structural YAML form

## SYNOPSIS

```
xml2yaml [FILE ...]
xml2yaml -h
xml2yaml --help
```

## DESCRIPTION

The `xml2yaml` tool converts XML into a structural YAML representation containing element names, attributes, and ordered child arrays.

The representation mirrors `xml2json` closely: each element has a `name`, an `attributes` mapping, and a `children` sequence. Non-blank text and CDATA nodes are emitted as `text` children.

## CURRENT CAPABILITIES

- emit element names and attributes
- emit ordered child arrays
- emit non-blank text and CDATA as text child objects
- quote scalar values to keep output deterministic and conservative

## OPTIONS

- `FILE` read XML input from FILE, or from standard input when omitted or `-`
- `-h`, `--help` print a short usage line

## LIMITATIONS

- Comments, processing instructions, and DOCTYPE declarations are not represented.
- Repeated child names remain repeated child objects rather than being grouped.
- There is no alternate XML-to-YAML convention mode yet.
- Entity references are not decoded before YAML output.

## EXAMPLES

```
xml2yaml document.xml
xml2yaml config.xml > config.yaml
```

## SEE ALSO

xml2json, xml2csv, xml2lines