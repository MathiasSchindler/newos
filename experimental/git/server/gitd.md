# GITD

## NAME

gitd - CORS-friendly smart-HTTP Git server for browser experiments

## SYNOPSIS

```
gitd [-b HOST] [-p PORT] [-r REPO_ROOT] [--once] [-q]
  [--tls-cert CERT --tls-key KEY]
  [--read-only] [--branches-only] [--no-delete-refs]
  [--max-body BYTES] [--max-wants N] [--max-haves N]
  [--max-ref-prefixes N] [--max-commands N]
  [--max-objects N] [--max-pack-bytes BYTES]
```

## DESCRIPTION

`gitd` serves local bare Git repositories over the Git smart-HTTP upload-pack and receive-pack protocols. It is a small dependency-free, no-libc server used by the browser Git WASM workbench and other transport experiments that need real Git clone, fetch, and push traffic without public-host CORS restrictions.

The server is dedicated to Git protocol traffic. It is not a static file server and is not based on the experimental `httpd` tool. It owns its HTTP request parsing, service routing, pkt-line handling, ref advertisement, pack generation, and CORS responses so the transport can stay focused and auditable.

Repositories are selected by URL path below `REPO_ROOT`. For example, if `REPO_ROOT` contains a bare repository at `example.git`, clients can clone it from `http://HOST:PORT/example.git`. A suffixless path such as `http://HOST:PORT/example` is also accepted as an alias for `example.git` when the direct path does not name a bare repository.

When `--tls-cert` and `--tls-key` are provided, `gitd` serves HTTPS directly using the in-tree TLS 1.3 and crypto code. Git smart HTTP remains HTTP/1.x inside the TLS stream; `gitd` does not negotiate or implement HTTP/2.

By default `gitd` binds to `0.0.0.0` and listens on port `8090`. This makes repositories reachable from any network interface allowed by the host firewall. Use `-b 127.0.0.1` for loopback-only testing.

## CURRENT CAPABILITIES

- serve bare repositories below a configured repository root
- advertise refs for smart HTTP with `GET /repo.git/info/refs?service=git-upload-pack` and `GET /repo.git/info/refs?service=git-receive-pack`
- respond to upload-pack requests with `POST /repo.git/git-upload-pack`
- respond to receive-pack requests with `POST /repo.git/git-receive-pack`
- accept branch creation, deletion, and strict fast-forward-only updates under `refs/heads/*`
- accept safe `refs/*` names for tags, notes, and custom namespaces; branch updates remain commit-only and fast-forward-only
- optionally run read-only, branches-only, or without delete-ref advertisement using receive-pack policy flags
- reject stale, forced branch updates, unsafe ref names, and object IDs that are not present in the received pack or repository
- update loose refs through `.lock` files and atomic rename; packed-ref deletions rewrite `packed-refs` through a lockfile
- generate upload packs from reachable objects using the in-tree Git object helpers, including `REF_DELTA` entries for later blobs that copy bounded matching spans from similar base blobs and insert changed bytes
- honor upload-pack `have` lines by excluding commits the client already reports as reachable
- advertise protocol v1 `multi_ack`, `multi_ack_detailed`, `side-band-64k`, `agent=newos-gitd`, and `symref=HEAD:...` when the repository exposes a symbolic HEAD
- advertise protocol v2 `ls-refs`, `fetch=shallow filter wait-for-done`, `object-info`, `bundle-uri`, `server-option`, and `object-format=sha1`
- advertise receive-pack `report-status`, `side-band-64k`, `delete-refs`, `no-thin`, and `agent=newos-gitd`
- return upload-pack results with `application/x-git-upload-pack-result`
- return receive-pack results with `application/x-git-receive-pack-result`
- support side-band pack responses when requested by the client
- decode gzip-compressed smart HTTP POST bodies, which canonical Git uses for larger protocol v2 fetch requests
- optionally serve HTTPS directly with TLS 1.3, X25519, AES-128-GCM-SHA256, and RSA-PSS-SHA256 certificate authentication
- enforce configurable limits for request bodies, wants, haves, ref prefixes, receive commands, object counts, and generated/received pack bytes
- use the project runtime I/O loop for listener and per-client request-read readiness
- include permissive CORS headers on all HTTP responses: `Access-Control-Allow-Origin: *`, methods `GET, HEAD, POST, OPTIONS`, and headers `content-type, authorization`
- log startup, requests, parse failures, receive-pack validation failures, TLS handshake failures, and other warnings/errors to the console unless `-q` is used
- answer browser preflight requests with `OPTIONS`
- expose `/health` and `/_status` endpoints that return `ok`
- optionally serve one accepted connection and exit with `--once`, which is useful for focused tests

## OPTIONS

- `-b HOST`, `--bind HOST` bind to `HOST`; defaults to `0.0.0.0`
- `-p PORT`, `--port PORT` listen on `PORT`; defaults to `8090`
- `-r REPO_ROOT`, `--repo-root REPO_ROOT` serve bare repositories below `REPO_ROOT`; defaults to the current directory
- `--tls-cert CERT` enable HTTPS using the certificate at `CERT`; PEM and DER certificates are accepted
- `--tls-key KEY` private RSA key for `--tls-cert`; PEM and DER PKCS#1 `RSA PRIVATE KEY` keys are accepted
- `--once` handle one accepted connection and then exit
- `-q`, `--quiet` suppress startup, request, warning, and error diagnostics
- `--read-only` disable receive-pack advertisement and writes
- `--branches-only` reject tags, notes, and custom ref namespaces during receive-pack
- `--no-delete-refs` do not advertise or accept delete-ref commands
- `--max-body BYTES` cap compressed or plain HTTP request bodies; defaults to 67108864
- `--max-wants N` cap upload-pack wants; defaults to 1024
- `--max-haves N` cap upload-pack haves; defaults to 4096
- `--max-ref-prefixes N` cap protocol v2 `ls-refs` prefixes; defaults to 64
- `--max-commands N` cap receive-pack ref commands; defaults to 64
- `--max-objects N` cap objects collected or accepted for one pack; defaults to 200000
- `--max-pack-bytes BYTES` cap generated or received pack bytes; defaults to 268435456
- `-h`, `--help` print the usage summary

## REPOSITORY LAYOUT

`gitd` expects bare repositories below `REPO_ROOT`. A URL path maps directly to a repository path below that root after percent-decoding and traversal checks. If that direct path is not a bare repository and the path does not already end in `.git`, `gitd` also tries the same path with `.git` appended.

For a root containing this tree:

```
repos/
  example.git/
  mona-isa.git/
```

the corresponding clone URLs are:

```
http://HOST:8090/example.git
http://HOST:8090/mona-isa.git
```

Serving working-tree checkouts is not a goal of this experiment. Use `git clone --bare` or another bare-repository creation path before pointing `gitd` at the repository root.

## PROTOCOL

The supported protocol surface is the smart-HTTP upload-pack and receive-pack path:

- `GET /REPO.git/info/refs?service=git-upload-pack` returns either a protocol v1 upload-pack service advertisement and refs or, when `Git-Protocol: version=2` is requested, a native protocol v2 capability advertisement
- `HEAD /REPO.git/info/refs?service=git-upload-pack` and `HEAD /REPO.git/info/refs?service=git-receive-pack` return the corresponding smart-HTTP discovery headers without a response body
- `POST /REPO.git/git-upload-pack` accepts protocol v1 fetch requests and protocol v2 `ls-refs`/`fetch` request bodies
- `GET /REPO.git/info/refs?service=git-receive-pack` returns the receive-pack service advertisement and refs
- `POST /REPO.git/git-receive-pack` accepts receive-pack commands followed by a pack, stores the pack, validates all ref commands, and returns report-status pkt-lines
- `OPTIONS /...` returns an empty CORS preflight response
- `GET /health` and `GET /_status` return `ok`

The server intentionally does not implement Git's legacy dumb HTTP static-object interface. Discovery requests without a `service=` query are rejected with a specific bad-request response; this is treated as expected behavior rather than a compatibility bug.

The HTTP layer is HTTP/1.x. HTTPS mode wraps the same HTTP/1.1 responses in TLS 1.3 and intentionally does not advertise ALPN or HTTP/2.

For fetches, the server records all `want` lines, builds the requested reachable object closure, excludes commits reachable from client `have` lines, and streams a single response pack. Protocol v2 fetch supports depth-limited shallow responses with `shallow-info` and `filter blob:none`; follow-up lazy blob fetches can request multiple blob wants in one round, including gzip-compressed POST bodies from canonical Git. When a protocol v2 fetch includes `have` lines, `gitd` sends an `acknowledgments` section before `packfile`. Protocol v2 `object-info` answers size requests for available objects, and `bundle-uri` returns an empty bundle list. Protocol v1 requests that stop before `done` report known `have` objects with `ACK ... common` and return `NAK` when no common object is known, so clients can continue negotiation with another stateless request. Receive-pack advertises `no-thin`, stores received objects, and applies updates transaction-like at the ref-validation level: every command must pass before any ref is written.

## LIMITATIONS

- no authentication or authorization; any reachable client can read repositories under `REPO_ROOT`, and write if receive-pack is enabled by policy
- HTTPS support is intentionally narrow: TLS 1.3 only, no TLS 1.2, no ALPN or HTTP/2, no client-certificate authentication, one certificate chain entry, and RSA PKCS#1 private keys only in the first pass
- local bare repositories only
- protocol v2 support covers the commands advertised by `gitd`; unknown future commands are rejected with an unsupported-command response
- multi-round negotiation remains stateless across HTTP requests, but v1 no-`done` rounds now report common `have` objects and can be have-only continuation probes
- blob delta generation is still smaller than full Git pack-objects: later blobs choose from a bounded set of previous blob bases and use sampled block matching, while commits, trees, tags, and unhelpful blob deltas are emitted whole
- request parsing is I/O-loop driven, but pack generation and receive-pack application still run synchronously once a complete request body has arrived
- request and pack limits are configurable but still enforced per complete request, not by streaming pack application

## EXAMPLES

Build the server:

```
make -C experimental/git/server
```

Serve local bare repositories on the default port:

```
experimental/git/server/build/gitd -r /path/to/repos
```

Use loopback-only binding for local browser testing:

```
experimental/git/server/build/gitd -b 127.0.0.1 -r /path/to/repos -p 8090
```

Clone with the project Git client or another smart-HTTP Git client:

```
build/macos-aarch64/git clone http://127.0.0.1:8090/example.git clone-out
git clone http://127.0.0.1:8090/example.git clone-out
```

Serve HTTPS directly with a local certificate and RSA private key:

```
experimental/git/server/build/gitd -r /path/to/repos -p 8443 \
  --tls-cert /path/to/cert.pem --tls-key /path/to/rsa-key.pem
git -c http.sslVerify=false -c http.version=HTTP/1.1 \
  clone https://127.0.0.1:8443/example.git clone-out
```

Push a fast-forward branch update, create tags or notes, or delete refs:

```
git -C clone-out push origin main
git -C clone-out push origin v1
git -C clone-out push origin :v1
```

Prepare and serve the larger `mona-isa` fixture used by the WASM workbench:

```
rm -rf tests/tmp/gitd-mona-repos
mkdir -p tests/tmp/gitd-mona-repos
git clone --bare https://github.com/MathiasSchindler/mona-isa.git \
  tests/tmp/gitd-mona-repos/mona-isa.git
experimental/git/server/build/gitd -r tests/tmp/gitd-mona-repos -p 8093
```

## SECURITY

`gitd` is intentionally permissive for browser experiments. CORS is open, authentication is absent, and the default bind address is network-reachable. Treat every repository below `REPO_ROOT` as public to any client that can connect to the selected host and port.

HTTPS protects the transport from passive network inspection, but it does not add authentication or repository authorization. For private local testing, use `-b 127.0.0.1`, choose a repository root that contains only intended bare repositories, and rely on host firewall rules when binding to a non-loopback interface.

## SEE ALSO

git, httpd, experimental/git/server/README.md, experimental/git/wasm/README.md