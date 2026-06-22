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

By default `gitd` binds to `0.0.0.0`, so it is reachable on network interfaces
allowed by the host firewall. Use `-b 127.0.0.1` for loopback-only testing, or
`-b ADDRESS` to choose a specific interface.

Then clone a bare repo below that root with a URL such as:

```sh
git clone http://HOST:8090/example.git clone-out
```

## Current Scope

- Smart HTTP discovery: `GET /repo.git/info/refs?service=git-upload-pack` and
  `GET /repo.git/info/refs?service=git-receive-pack`.
- Smart HTTP fetch: `POST /repo.git/git-upload-pack`.
- Smart HTTP push: `POST /repo.git/git-receive-pack`.
- Local bare repositories only.
- Full-history, undeltified pack generation using the existing newos Git object
  and pack helpers.
- Upload-pack `have` lines exclude commits the client already reports as
  reachable.
- Receive-pack accepts branch creation and strict fast-forward-only
  `refs/heads/*` updates, and rejects stale, forced, deletion, and non-branch
  ref updates.
- Listener and per-client request reads use the project runtime I/O loop.
- CORS headers on all responses:
  `Access-Control-Allow-Origin: *`, `GET, POST, OPTIONS`, and
  `content-type, authorization`.
- `OPTIONS` preflight support for browser callers.

## Limits

- No authentication. Treat the selected repository root as publicly readable to
  any client that can reach the bound address, and writable by any client that
  can form an accepted fast-forward receive-pack request.
- No protocol v2, shallow clone, filters, or multi-round negotiation.
- The server accepts the first `want` and returns the reachable object closure.
- Receive-pack does not support ref deletion, tag updates, notes, or non-branch
  namespaces.
- Pack generation and receive-pack application still run synchronously after a
  complete request body has arrived.
- Bare repositories are expected. Serving working-tree checkouts is not a goal
  for this experiment.

This is the CORS-friendly Git transport counterpart to the browser WASM
workbench in `experimental/git/wasm`.