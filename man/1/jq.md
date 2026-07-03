# JQ

## NAME

jq - small JSON selector

## SYNOPSIS

```
jq [-r] FILTER [FILE]
```

## DESCRIPTION

`jq` implements a deliberately small subset of jq-style JSON selection for
dependency-free scripts. Supported filters are:

- `.` - print the full JSON value
- `.key` - print an object field
- `.key.subkey` - print nested object fields
- `."key-name"` and `["key-name"]` - select object fields whose names are not simple identifiers
- `.array[0]` - print an array element by zero-based index
- `.array[-1]` - print an array element by negative index from the end
- `.array[start:end]` - print an array slice, with omitted or negative bounds allowed
- `.array[]` - print each array element, one JSON value per line
- `.array[].key` - combine array iteration with later object-field selection
- `.key?` and `.array[0]?` - return `null` instead of failing when an access is missing
- `left | right` - pipe each value produced by `left` into `right`
- `left, right` - emit the results of both filters
- `left // right` - evaluate `right` when `left` is missing, `null`, `false`, or empty
- `if condition then filter else filter end` - choose between two filters
- `try filter catch fallback` and `filter?` - suppress filter failures or emit a fallback value
- `empty` - emit no result
- `select(filter)` - keep the input value when `filter` produces a truthy value
- `map(filter)` - apply a filter to each array element and return an array of the results
- `reduce stream as $name (init; update)` - fold a stream into one value
- `foreach stream as $name (init; update)` - emit each intermediate accumulator value
- `del(path, ...)` - delete object fields or array elements by path
- `to_entries`, `from_entries`, and `with_entries(filter)` - convert objects or arrays to entry arrays and back
- `sort`, `sort_by(filter)`, `min`, `max`, `min_by(filter)`, and `max_by(filter)` - order arrays by value or by a derived key
- `add`, `first`, `last`, `unique`, `unique_by(filter)`, `group_by(filter)`, `flatten`, `any(filter)`, `all(filter)`, and `range(...)` - provide common array, stream, and generator operations
- `index(value)` and `indices(value)` - find matching array elements or string substrings
- `contains(string)`, `startswith(string)`, and `endswith(string)` - test string content
- `split(string)` and `join(string)` - split a string or join array values into a string
- `test(regex)`, `match(regex)`, `capture(regex)`, `sub(regex; replacement)`, and `gsub(regex; replacement)` - use the shared regular-expression engine for tests, simple match objects, and replacement
- `length` - return the length of an array, object, or string
- `keys` - return array indexes or object keys in input order
- `has("key")` and `has(index)` - test whether an object field or array index exists
- `type` - print the JSON type name as a string
- `tonumber`, `tostring`, `fromjson`, `tojson`, `ascii_upcase`, and `ascii_downcase` - perform small conversions
- `@json`, `@text`, and `@string` - format the current value as a JSON string
- `..` - recursively emit the current value and all descendant values
- integer arithmetic with `+`, `-`, `*`, `/`, and `%`
- string concatenation with `+`
- ordering and equality comparisons with `==`, `!=`, `<`, `<=`, `>`, and `>=`
- boolean operators `and`, `or`, and unary `not`
- string interpolation such as `"user=\(.name)"`
- array construction such as `[.name, .meta.count]`
- object construction such as `{name: .name, count: .meta.count}`, shorthand fields such as `{name}`, and computed keys such as `{(.key): .value}`
- variable binding with `filter as $name | filter` and destructuring patterns
	such as `. as {name: $name, users: [$first]} | filter`
- user-defined functions such as `def inc($x): $x + 1; inc(.count)`, including
	access to active `as` bindings at the call site
- leading exact-path `include "file";` and `import "file" as name;` directives for loading def-only helper files
- path assignment and update, such as `.count = 3` and `.count |= . + 1`, with
	object/array path creation and comma-separated target paths

`select` also supports simple equality predicates such as
`select(.name == "Ada")` and `select(.active != false)`.

FILE defaults to standard input.

## OPTIONS

- `-r`, `--raw-output` - for string results, print the string content without surrounding quotes.
- `-h`, `--help` - show usage.

## LIMITATIONS

This is not full jq. The supported filter forms are listed in DESCRIPTION, and
the command-line flags are listed in OPTIONS. The current limitations are:

- Arithmetic is integer-only, so floating-point operations are not supported.
- String processing is intentionally small. Interpolation, `contains`,
	`startswith`, `endswith`, `split`, `join`, `ascii_upcase`, `ascii_downcase`,
  `tostring`, `tojson`, `fromjson`, `test`, `match`, `capture`, `sub`, `gsub`,
  `@json`, and `@text` are supported, but jq's broader string, regex flag,
  named-capture, escaping, and formatting libraries are not implemented.
- User-defined functions are still limited to simple `def` declarations with
	fixed arguments. They can read active bindings at the call site, and leading
	`include`/`import` directives can load exact-path files containing definitions,
	but namespacing, search paths, module metadata, exports, and full jq closure
	semantics are not implemented.
- Variable binding supports `$name`, object destructuring, and array
	destructuring, but it does not support default values, alternative patterns, or
	all jq destructuring forms.
- Assignment and update create missing object fields and array elements, filling
	skipped array positions with `null`, and comma-separated target paths are
	applied left to right. More advanced assignment forms, destructuring assignment,
	and delete-style update operators are not implemented.
- Entry conversion, deletion, sorting, grouping, aggregation, and JSON
	conversion cover common object, array, string, boolean, null, and integer
	cases, but they do not implement every alias, collation rule, or type coercion
	from full jq.
- Update operators other than `=` and `|=` are not implemented.
- Control-flow supports `if ... then ... else ... end`, `empty`, `try`,
	`catch`, `reduce`, and `foreach`, but labels, `break`, and the full jq
	control-flow grammar are not implemented.
- Slices are supported for arrays, but string and object slicing are not
	implemented.
- Optional access with `?` returns `null` for missing object fields,
	out-of-range array indexes, and type mismatches; it does not implement full
	jq-compatible missing-value propagation. Suffix `?` on non-path filters
	suppresses failures by producing an empty result.
- Values selected directly from the input are emitted as the original input
	slice rather than as normalized or pretty-printed JSON.
- Generated arrays, objects, mapped values, and updated documents are emitted in
	compact form only.
- `keys` preserves input object order rather than sorting keys as full jq does.
- `map` requires an array input.
- Missing fields, malformed JSON, unsupported filters, and out-of-range indexes
	report a filter failure in direct path selection instead of producing full
	jq-compatible `null` or empty-stream behavior.
- Raw output with `-r` decodes JSON string escapes for string results, including
	Unicode escapes, but non-string results are still emitted as JSON.

## JSON Output

JSON mode limitation: `jq` does not use the shared JSON Lines envelope for normal output because its
normal output is already JSON (or raw string data with `-r`). Diagnostics and
usage remain plain text in this initial version.

## SEE ALSO

xml2json, json-output
