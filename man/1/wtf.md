# WTF

## NAME

wtf - Wikipedia Terminal Facts summary lookup

## SYNOPSIS

```
wtf [-T TIMEOUT] [--base-url URL] [--url] TERM...
```

## DESCRIPTION

`wtf` looks up a short Wikipedia-style summary for a term and prints the title,
description, and extract in the terminal. It uses the project platform networking
layer directly and does not depend on libc networking helpers or external JSON
libraries.

The default endpoint is the Wikipedia REST summary endpoint:

```
http://en.wikipedia.org/api/rest_v1/page/summary/TERM
```

The term is URL-encoded and spaces are written as underscores.

## OPTIONS

- `-T TIMEOUT` / `--timeout TIMEOUT` - set the network timeout; accepts duration
  values such as `500ms`, `2s`, or `1.5m`
- `--base-url URL` - use an alternate REST summary endpoint base. This is useful
  for local mirrors, tests, and future HTTPS-capable transports.
- `--url` - print the page URL when the response contains one
- `-h` / `--help` - show help

## LIMITATIONS

Wikipedia currently redirects the public HTTP endpoint to HTTPS. The imported TLS
client is available to `mail`, but `wtf` has not been switched to it yet; direct
live Wikipedia lookups report that HTTPS is required. The tool is otherwise
functional with plain HTTP Wikipedia-compatible summary endpoints, such as a
local mirror or test server.

The JSON parser is intentionally narrow: it extracts the `title`, `description`,
`extract`, and `page` string fields used by the REST summary response.

## EXAMPLES

```
wtf C programming language
wtf --url Ada Lovelace
wtf --base-url http://127.0.0.1:8080/api/rest_v1/page/summary Plan 9
```

## SEE ALSO

wget, nslookup, dig
