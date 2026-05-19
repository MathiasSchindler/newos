# lsof(1)

## Name

lsof - list open files

## Synopsis

`lsof [-p PID] [PATH ...]`

## Description

`lsof` lists open file descriptors as `COMMAND PID USER FD NAME` rows.

On platforms that expose `/proc/<pid>/fd`, each file descriptor symlink is read and printed. Path arguments filter rows by substring match against `NAME`.

## Options

- `-p PID` show open files for one process
- `-h`, `--help` show usage information

## Examples

`lsof -p 1`

`lsof /var/log`

## Limitations

Systems without `/proc/<pid>/fd` support may only print the header and return a non-zero status because the platform does not expose open-file enumeration yet.

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

