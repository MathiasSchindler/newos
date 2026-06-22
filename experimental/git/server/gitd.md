# GITD

## NAME

gitd - CORS-friendly smart-HTTP Git server for browser experiments

## SYNOPSIS

```
gitd [-b HOST] [-p PORT] [-r REPO_ROOT] [--once] [-q]
```

## DESCRIPTION

`gitd` serves local bare Git repositories over the Git smart-HTTP upload-pack and receive-pack protocols. It is a small dependency-free, no-libc server used by the browser Git WASM workbench and other transport experiments that need real Git clone, fetch, and push traffic without public-host CORS restrictions.

The server is dedicated to Git protocol traffic. It is not a static file server and is not based on the experimental `httpd` tool. It owns its HTTP request parsing, service routing, pkt-line handling, ref advertisement, pack generation, and CORS responses so the transport can stay focused and auditable.

Repositories are selected by URL path below `REPO_ROOT`. For example, if `REPO_ROOT` contains a bare repository at `example.git`, clients can clone it from `http://HOST:PORT/example.git`.

By default `gitd` binds to `0.0.0.0` and listens on port `8090`. This makes repositories reachable from any network interface allowed by the host firewall. Use `-b 127.0.0.1` for loopback-only testing.

## CURRENT CAPABILITIES

- serve bare repositories below a configured repository root
- advertise refs for smart HTTP with `GET /repo.git/info/refs?service=git-upload-pack` and `GET /repo.git/info/refs?service=git-receive-pack`
- respond to upload-pack requests with `POST /repo.git/git-upload-pack`
- respond to receive-pack requests with `POST /repo.git/git-receive-pack`
- accept branch creation and strict fast-forward-only updates under `refs/heads/*`
- reject stale, forced, deletion, and non-branch ref updates
- generate undeltified packs from reachable objects using the in-tree Git object and pack helpers
- honor upload-pack `have` lines by excluding commits the client already reports as reachable
- advertise `multi_ack`, `multi_ack_detailed`, `side-band-64k`, `agent=newos-gitd`, and `symref=HEAD:...` when the repository exposes a symbolic HEAD
- advertise receive-pack `report-status`, `side-band-64k`, and `agent=newos-gitd`
- return upload-pack results with `application/x-git-upload-pack-result`
- return receive-pack results with `application/x-git-receive-pack-result`
- support side-band pack responses when requested by the client
- use the project runtime I/O loop for listener and per-client request-read readiness
- include permissive CORS headers on all HTTP responses: `Access-Control-Allow-Origin: *`, methods `GET, POST, OPTIONS`, and headers `content-type, authorization`
- answer browser preflight requests with `OPTIONS`
- expose `/health` and `/_status` endpoints that return `ok`
- optionally serve one accepted connection and exit with `--once`, which is useful for focused tests

## OPTIONS

- `-b HOST`, `--bind HOST` bind to `HOST`; defaults to `0.0.0.0`
- `-p PORT`, `--port PORT` listen on `PORT`; defaults to `8090`
- `-r REPO_ROOT`, `--repo-root REPO_ROOT` serve bare repositories below `REPO_ROOT`; defaults to the current directory
- `--once` handle one accepted connection and then exit
- `-q`, `--quiet` suppress the startup listening message
- `-h`, `--help` print the usage summary

## REPOSITORY LAYOUT

`gitd` expects bare repositories below `REPO_ROOT`. A URL path maps directly to a repository path below that root after percent-decoding and traversal checks.

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

The supported protocol surface is the version-one smart-HTTP upload-pack and receive-pack path:

- `GET /REPO.git/info/refs?service=git-upload-pack` returns the upload-pack service advertisement and refs
- `POST /REPO.git/git-upload-pack` accepts an upload-pack request body and returns a pack containing objects reachable from the first wanted commit
- `GET /REPO.git/info/refs?service=git-receive-pack` returns the receive-pack service advertisement and refs
- `POST /REPO.git/git-receive-pack` accepts receive-pack commands followed by a pack, stores the pack, validates all ref commands, and returns report-status pkt-lines
- `OPTIONS /...` returns an empty CORS preflight response
- `GET /health` and `GET /_status` return `ok`

The server accepts the first `want` object in the upload-pack request, builds the reachable object closure, excludes commits reachable from client `have` lines, and streams a single response pack. Receive-pack updates are transaction-like at the ref-validation level: every command must pass before any ref is written. It does not implement multi-round ACK negotiation, shallow boundaries, protocol v2, filters, or partial clone.

## LIMITATIONS

- receive-pack push is limited to branch creation and strict fast-forward-only `refs/heads/*` updates
- ref deletion, tags, notes, and non-branch namespaces are rejected
- no authentication or authorization; any reachable client can read repositories under `REPO_ROOT`
- plain HTTP only; TLS termination must happen outside `gitd` if needed
- local bare repositories only
- no protocol v2, shallow clone, filters, or multi-round ACK negotiation
- no delta pack generation; packs are intentionally simple and undeltified
- request parsing is I/O-loop driven, but pack generation and receive-pack application still run synchronously once a complete request body has arrived
- request bodies are capped by the fixed server body limit

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

Push a fast-forward branch update:

```
git -C clone-out push origin main
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

For private local testing, use `-b 127.0.0.1`, choose a repository root that contains only intended bare repositories, and rely on host firewall rules when binding to a non-loopback interface.

## SEE ALSO

git, httpd, experimental/git/server/README.md, experimental/git/wasm/README.md