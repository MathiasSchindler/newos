# WGET

## NAME

wget - fetch data from HTTP, HTTPS, or local file URLs

## SYNOPSIS

```
wget [-q] [-S] [-T TIMEOUT] [-O FILE] URL...
```

## DESCRIPTION

`wget` retrieves content from a URL and writes it to a local file or to standard
output. In the current implementation it is a small freestanding fetch tool for
simple `http://` and `https://` downloads and `file://` copies, built without
external dependencies or a hosted libc requirement in the tool itself.

## CURRENT CAPABILITIES

- fetch from plain `http://` URLs
- fetch from `https://` URLs through the shared freestanding TLS client
- copy from `file://` URLs
- save to a chosen file with `-O FILE`
- stream the fetched content to stdout with `-O -`
- quiet mode with `-q`
- show server response headers with `-S`
- apply a network timeout with `-T`
- follow a small number of HTTP redirects
- stop after the declared `Content-Length` bytes when that header is present

## OPTIONS

- `-q` / `--quiet` - reduce status output
- `-S` / `--server-response` - print the received HTTP response headers to
  stderr
- `-T TIMEOUT` / `--timeout TIMEOUT` - set an idle socket timeout; accepts the
  same duration syntax used by other tools such as `250ms`, `2s`, or `1.5m`.
  The default is 30 seconds.
- `-O FILE` - write the result to FILE; use `-O -` to write to stdout

## LIMITATIONS

- TLS peer certificates are verified where the platform TLS layer has trust
  anchor support; Windows freestanding HTTPS currently reports an unverified
  peer status. Hosted POSIX builds support `NEWOS_NATIVE_TLS_INSECURE=1` to
  bypass certificate verification for diagnostics.
- no resume/download continuation support
- no authentication, cookies, proxy support, or recursive mirroring
- only simple GET-style retrieval is supported
- conflicting or malformed `Content-Length` headers are rejected
- redirects with unsafe control characters, spaces, unsupported schemes, or
  oversized locations are rejected instead of being followed after truncation

## EXAMPLES

```
wget http://example.com/
wget https://example.com/
wget -O page.html http://example.com/index.html
wget -q -O - file:///tmp/data.txt
wget -S -T 2s http://127.0.0.1:8080/status
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

netcat, cat, cp
