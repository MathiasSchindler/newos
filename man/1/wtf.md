# WTF

## NAME

wtf - Wikipedia Terminal Facts summary lookup

## SYNOPSIS

```
wtf [-l LANG] [-T TIMEOUT] [--base-url URL] [--url] [--json] [--color[=WHEN]] TERM...
```

## DESCRIPTION

`wtf` looks up a short Wikipedia-style summary for a term and prints the title,
description, and extract in the terminal. The title is rendered in bold when
standard output is a capable terminal, and the extract is wrapped at word
boundaries to the output width. It uses the project platform networking layer
directly and does not depend on libc networking helpers or external JSON libraries.

The default endpoint is the Wikipedia REST summary endpoint:

```
https://en.wikipedia.org/api/rest_v1/page/summary/TERM
```

The term is URL-encoded and spaces are written as underscores.

When the server sends a `Content-Length` header, `wtf` stops reading as soon as
that many body bytes have arrived instead of waiting for the peer to close the
connection. Plain ASCII extracts use a fast wrapping path; non-ASCII text still
falls back to the shared Unicode display-width logic.

## OPTIONS

- `-l LANG` / `--language LANG` / `--lang LANG` - use a two-letter Wikipedia
  language code, such as `de`, `fr`, or `en`. This changes the default endpoint
  to `https://LANG.wikipedia.org/api/rest_v1/page/summary/TERM`.
- `-T TIMEOUT` / `--timeout TIMEOUT` - set the network timeout; accepts duration
  values such as `500ms`, `2s`, or `1.5m`
- `--base-url URL` - use an alternate REST summary endpoint base. This is useful
  for local mirrors and tests. When set, it is used instead of the language-based
  default endpoint.
- `--url` - print the page URL when the response contains one
- `--json` - write the summary as a newline-delimited JSON event
- `--no-title` - do not print the article title
- `--no-description` - do not print the one-line description
- `--no-extract` - do not print the introduction extract
- `--only-title` - print only the article title
- `--only-description` - print only the one-line description
- `--only-extract` - print only the introduction extract
- `--color[=WHEN]` / `--colour[=WHEN]` - control styled output. `WHEN` may be
  `auto`, `always`, or `never`. With no value, color is forced on.
- `-h` / `--help` - show help

## LIMITATIONS

HTTPS is handled by the project-native TLS client. Certificate verification uses
the platform trust bundle when one can be found. Set `NEWOS_NATIVE_TLS_INSECURE=1`
only for controlled debugging against test endpoints.

Public lookups still include DNS, TCP, TLS, certificate verification, server
processing, and data-transfer time. Those external costs usually dominate short
summaries.

HTTP redirects are reported but not followed automatically yet.

The JSON parser is intentionally narrow: it extracts the `title`, `description`,
`extract`, and `page` string fields used by the REST summary response.

## EXAMPLES

```
wtf C programming language
wtf -l de Microsoft
wtf --only-description Ada Lovelace
wtf --no-title --no-description C
wtf --color=never Microsoft
wtf --url Ada Lovelace
wtf --base-url http://127.0.0.1:8080/api/rest_v1/page/summary Plan 9
```

## JSON Output

With `--json`, `wtf` writes one JSON Lines event using the common envelope documented in `json-output`. The event name is `wtf_summary` and `data` contains:

- `term`: joined lookup term
- `language`: two-letter language code used for the default endpoint
- `request_url`: URL requested by the tool
- `title`: response title string, or `null`
- `description`: response description string, or `null`
- `extract`: response extract string, or `null`
- `page_url`: page URL string, or `null`
- `missing`: boolean MediaWiki not-found indicator

JSON mode ignores text-display selection flags such as `--only-title` and emits all parsed summary fields. Diagnostics and usage errors are emitted on stderr using the shared JSON diagnostic envelope when JSON mode is enabled.

## SEE ALSO

wget, nslookup, dig
