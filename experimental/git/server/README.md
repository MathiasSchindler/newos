# newos Git Server Experiment

This directory contains a small dependency-free, no-libc Git smart-HTTP server
for browser and network experiments. It intentionally adds permissive CORS
headers so a browser-hosted Git client can exercise real Git clone, fetch, and
push traffic without a GitHub-style cross-origin block.

`gitd` is a dedicated Git protocol server, not an extension of the project's
experimental `httpd` tool. It owns its request parsing, Git service routing, and
pkt-line/pack responses so it can evolve around Git transport needs rather than
static-file serving constraints.

Build it with:

```sh
make -C experimental/git/server
```

On local macOS/aarch64 this produces a project-linked Mach-O binary at
`experimental/git/server/build/gitd`. On Linux it follows the freestanding Linux
path used by the other experiments.

Run it against a directory of bare repositories:

```sh
experimental/git/server/build/gitd -r /path/to/repos -p 8090
```

To serve HTTPS directly, pass a certificate and RSA PKCS#1 private key. Git smart
HTTP is still HTTP/1.x inside TLS; `gitd` does not implement HTTP/2.

```sh
experimental/git/server/build/gitd -r /path/to/repos -p 8443 \
  --tls-cert /path/to/cert.pem --tls-key /path/to/rsa-key.pem
```

By default `gitd` binds to `0.0.0.0`, so it is reachable on network interfaces
allowed by the host firewall. Use `-b 127.0.0.1` for loopback-only testing, or
`-b ADDRESS` to choose a specific interface.

Then clone a bare repo below that root with a URL such as:

```sh
git clone http://HOST:8090/example.git clone-out
git -c http.sslVerify=false -c http.version=HTTP/1.1 \
  clone https://HOST:8443/example.git clone-out
```

## Current Scope

- Smart HTTP discovery: `GET /repo.git/info/refs?service=git-upload-pack` and
  `GET /repo.git/info/refs?service=git-receive-pack`.
- Smart HTTP fetch: protocol v1 upload-pack plus native protocol v2 `ls-refs`,
  `fetch`, `object-info`, and `bundle-uri` over `POST /repo.git/git-upload-pack`.
- Smart HTTP push: `POST /repo.git/git-receive-pack`.
- Local bare repositories only.
- Pack generation using the existing newos Git object helpers, including blob
  `REF_DELTA` entries that copy bounded matching spans from a bounded set of
  similar base blobs and insert changed bytes when doing so is useful.
- Upload-pack `have` lines exclude commits the client already reports as
  reachable; protocol v1 no-`done` rounds report known haves with `ACK ... common`.
- Protocol v2 fetch supports shallow depth requests, `filter blob:none`, and
  multi-want follow-up fetches for lazy blob checkout, including gzip-compressed
  POST bodies sent by canonical Git for larger requests; v2 `object-info` answers
  size queries and `bundle-uri` returns an empty bundle list.
- Receive-pack accepts branch creation, deletion, strict fast-forward-only
  `refs/heads/*` updates, and safe tags, notes, and custom `refs/*`
  namespaces. Branch refs remain commit-only and fast-forward-only.
- Receive-pack policy flags can disable writes, deletes, or non-branch
  namespaces; loose ref writes use `.lock` files and atomic rename.
- Request, negotiation, object-count, and pack-byte limits are configurable with
  `--max-*` flags.
- Optional HTTPS listener mode uses the project TLS 1.3 and crypto code with
  X25519, AES-128-GCM-SHA256, and RSA-PSS-SHA256 certificate authentication.
- Listener and per-client request reads use the project runtime I/O loop.
- CORS headers on all responses:
  `Access-Control-Allow-Origin: *`, `GET, POST, OPTIONS`, and
  `content-type, authorization`.
- `OPTIONS` preflight support for browser callers.

## Limits

- No authentication. Treat the selected repository root as publicly readable to
  any client that can reach the bound address, and writable by any client that
  can form an accepted receive-pack request unless policy flags disable writes.
- HTTPS is TLS 1.3 only, with no TLS 1.2, ALPN, HTTP/2, or client certificate
  authentication. The first pass accepts one PEM/DER certificate and an RSA
  PKCS#1 private key.
- Protocol v2 support covers the commands advertised by `gitd`; unknown future
  commands are rejected as unsupported requests.
- Multi-round negotiation is stateless across HTTP requests, but v1 upload-pack
  requests that stop before `done` now report common haves and can be have-only
  continuation probes.
- Pack generation and receive-pack application still run synchronously after a
  complete request body has arrived.
- Bare repositories are expected. Serving working-tree checkouts is not a goal
  for this experiment.

This is the CORS-friendly Git transport counterpart to the browser WASM
workbench in `experimental/git/wasm`.