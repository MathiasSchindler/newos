# WTF

## NAME

wtf - Wikipedia Terminal Facts summary lookup

## SYNOPSIS

```
wtf [-l LANG] [-T TIMEOUT] [--base-url URL] [--url] [--color[=WHEN]] TERM...
```

## DESCRIPTION

`wtf` looks up a short Wikipedia-style summary for a term and prints the title,
description, and extract in the terminal. The title is rendered in bold when
standard output is a capable terminal. It uses the project platform networking
layer directly and does not depend on libc networking helpers or external JSON
libraries.

The default endpoint is the Wikipedia REST summary endpoint:

```
https://en.wikipedia.org/api/rest_v1/page/summary/TERM
```

The term is URL-encoded and spaces are written as underscores.

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

## SEE ALSO

wget, nslookup, dig
