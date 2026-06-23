# Git WASM experiment

This directory is an untracked experiment for compiling the real `src/tools/git.c`
client to `wasm32-unknown-unknown` and driving it from a browser UI.

The first milestone is intentionally offline:

- compile the actual Git command dispatcher, object store, index, diff, and commit
  code into a no-libc WebAssembly module
- provide a small browser-backed virtual filesystem for a repository rooted at
  `/repo`
- expose JS helpers to write worktree files, run `git` commands, and read stdout,
  stderr, and files back from the module
- demonstrate a small browser workbench with a file explorer, multiple open
  editor tabs, local change list, commit controls, and command output
- highlight Python, C/C header, and Markdown files in the browser editor with a
  dependency-free overlay module
- show recent commit history in a dedicated panel, with click-through commit
  details from `git show --stat`
- clone from the experimental `gitd` smart-HTTP server by using browser `fetch()`
  for the network leg and handing the returned pack to the real Git code for
  storage and checkout

General socket/TLS transport is still stubbed. The current network path is a
targeted async smart-HTTP bridge for CORS-enabled servers such as
`experimental/git/server/gitd`; ordinary public Git hosting endpoints may still
be blocked by CORS.

## Build

From the repository root:

```sh
make -C experimental/git/wasm
```

The build uses Clang directly and emits `build/git.wasm` under this experiment
folder. No Emscripten or WASI runtime is required.

Run the offline smoke test with:

```sh
make -C experimental/git/wasm test
```

With `gitd` serving `example.git`, run the client/server smoke with:

```sh
GITD_URL=http://127.0.0.1:8090/example.git make -C experimental/git/wasm test-gitd
```

## Try It

Serve this directory with any static file server and open `index.html`:

```sh
python3 -m http.server 8080 --directory experimental/git/wasm
```

The page loads `build/git.wasm` and runs Git commands inside the module against
its in-memory repository. The initial demo repo is created in `/repo`; the file
explorer can open several files, edit them in tabs, save them back into the
virtual filesystem, and stage/commit through the real Git client.

To test real client/server traffic, build and run `gitd` against a directory of
bare repositories, then enter the repository URL in the top bar and press
`Clone`:

```sh
make -C experimental/git/server test
experimental/git/server/build/gitd -r /path/to/repos -p 8090
```

The default URL in the UI is `http://127.0.0.1:8090/example.git`; replace the
host, port, and repository name to match your running server.

For a larger fixture, mirror `mona-isa` into a local bare repository and serve it
on port 8093:

```sh
rm -rf tests/tmp/gitd-mona-repos
mkdir -p tests/tmp/gitd-mona-repos
git clone --bare https://github.com/MathiasSchindler/mona-isa.git \
  tests/tmp/gitd-mona-repos/mona-isa.git
experimental/git/server/build/gitd -r tests/tmp/gitd-mona-repos -p 8093
```

Then press `Mona Repo` in the top bar, or enter
`http://127.0.0.1:8093/mona-isa.git` manually and press `Clone`.

## Current Scope

Supported in the browser shim:

- regular files and directories
- stdout/stderr capture
- current directory and environment lookups
- enough file metadata for Git's index/status/commit flows
- file-tree export to JavaScript for the browser explorer
- syntax highlighting for Python, C/C header, and Markdown files
- recent history rendering from `git log --format`, plus `git show --stat`
  details for selected commits
- smart-HTTP clone from `gitd` into `/repo`

Stubbed for now:

- sockets, TLS, DNS, and HTTP transport
- `pull` still uses the ordinary Git command path and currently reports
  transport failures because generic browser sockets/TLS are not implemented yet
- process spawning, hooks, editors, and credential helpers
- symlinks and hard links
- persistent storage across page reloads
