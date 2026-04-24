# server(7)

## NAME

server - design and implementation guidance for network daemons in newos

## OVERVIEW

This page is intentionally both a **placement guide** and a **design guide**.
It should be read together with:

- `man/7/project-layout.md`
- `man/7/platform.md`
- `man/7/runtime.md`
- `man/7/build.md`
- `man/7/userland.md`

The repository rules from those pages still apply:

- one public tool entry point in `src/tools/`
- private implementation in `src/tools/<tool>/`
- genuinely reusable cross-tool code in `src/shared/`
- OS interaction through `src/shared/platform.h` and the backends in `src/platform/`

This document adds the architectural decisions that should **not** be left vague when internet-facing daemons are introduced.

Where necessary, it distinguishes between:

- what already exists in the tree today,
- what is only partially present,
- and what still needs to be implemented.

---

## CURRENT STATUS IN THE TREE

### Already present today

The repository already contains the following relevant building blocks:

- hosted POSIX networking and process support in `src/platform/posix/`
- freestanding Linux networking and process support in `src/platform/linux/`
- a working SSH client under `src/tools/ssh/`
- a minimal SSH daemon under `src/tools/sshd/` with password-authenticated exec sessions
- a small hosted static HTTP daemon under `src/tools/httpd/`
- a small config-driven service supervisor under `src/tools/service/`
- shared crypto code in `src/shared/crypto/`
- shared server-oriented config and log helpers under `src/shared/`
- hosted macOS compatibility through the POSIX backend, as described in `platform(7)` and `project-layout(7)`

In practical terms, this means the project already has enough runtime and platform surface to support client networking, polling, subprocess management, and a substantial portion of the cryptographic groundwork.

### Not yet present as first-class subsystems

The following are still design targets rather than finished repository subsystems:

- no `httpsd` daemon yet
- no PTY-backed SSH session subsystem yet
- no shared `src/shared/tls/` subsystem yet
- no shared `src/shared/http/` subsystem yet
- no small server-oriented account database layer yet
- no full standalone `sv`-style dependency manager, launchd integration layer, or richer service-control stack yet

The rest of this page should be read in that light: some sections describe current reality, while others define the intended shape of the next implementation steps.

---

## SCOPE AND NON-GOALS

The intended direction is:

- small and auditable daemons,
- explicit state machines,
- minimal but real reuse,
- strong bias toward predictable behavior over framework cleverness.

The project should **not** attempt to build a giant generic application-server framework. That would increase code size, make security review harder, and blur the ownership rules described in `project-layout(7)`.

However, this does **not** mean that every daemon should invent its own architecture from scratch. A few decisions should be shared from day one.

---

## SOURCE-TREE DOCKING

The current repository already provides the core layers needed for server work:

- `src/tools/` for user-facing commands and daemon entry points
- `src/tools/ssh/` as an example of a larger tool with private internal modules
- `src/shared/runtime/` for memory, string, parse, and I/O support
- `src/shared/tool_io.c`, `tool_cli.c`, `tool_fs.c`, and `tool_path.c` for cross-tool helper logic
- `src/shared/crypto/` for reusable cryptographic primitives
- `src/shared/platform.h` for the platform boundary
- `src/platform/posix/` for hosted development and testing
- `src/platform/linux/` for freestanding Linux support
- `src/tools/init.c`, `getty.c`, and `shutdown.c` for service-adjacent system tools

Server code should therefore dock into the tools, shared, and platform layers. It should **not** live in `src/compiler/` except when new shared sources must be registered in `src/compiler/source_manifest.h` for build integration.

### Tool-private code

Keep the following in a daemon-owned directory such as `src/tools/httpsd/`, `src/tools/sshd/`, or `src/tools/ircd/`:

- protocol parsers
- connection state machines
- request routing
- daemon-specific configuration logic
- local policy and authorization decisions
- application handlers

### Shared code

Promote code into `src/shared/` only when it is both real and justified. Good candidates are:

- TLS primitives and record helpers
- generic HTTP request and header parsing helpers
- shared account / key / certificate parsing helpers
- bounded connection-buffer or ring-buffer helpers
- small config parsing helpers reused by multiple daemons
- shared log escaping and structured logging helpers

### Platform code

Anything that depends on OS interfaces still belongs behind `platform.h`, backed by the existing files in:

- `src/platform/posix/net.c`
- `src/platform/posix/process.c`
- `src/platform/posix/fs.c`
- `src/platform/linux/net.c`
- `src/platform/linux/process.c`
- `src/platform/linux/fs.c`

If a capability matters in both hosted and freestanding builds, add it to the platform surface and implement both backends, as described in `platform(7)`.

---

## CONCURRENCY MODEL

This is a first-order architectural decision and should not be left implicit.

### Default project policy

For **HTTP, HTTPS, IRC, NNTP, and specialized API services**, the default model should be:

- one process,
- non-blocking sockets,
- an event loop based on poll-style readiness,
- explicit per-connection state objects,
- hard limits on concurrent connections, header size, body size, and idle time.

This model fits the existing project style well:

- small code size
- predictable memory footprint
- no mandatory threading substrate
- good compatibility with both the hosted POSIX and freestanding Linux paths

### Exceptions

**SSH should be treated differently.** An SSH daemon benefits from stronger isolation and should prefer:

- a small privileged listener or monitor,
- followed by a separate unprivileged session process for each authenticated session or channel group,
- especially when PTY or shell execution is involved.

This is both a security and a simplicity choice. SSH session handling often becomes easier to reason about once the interactive child is isolated from the network-facing control path.

### What is explicitly not the default

The document does **not** recommend thread-per-connection as the baseline design. It is not forbidden, but it would pull in a larger runtime and scheduling story than the project currently has.

If a daemon later needs worker processes for expensive or blocking operations, those should be introduced deliberately and documented as exceptions rather than becoming the hidden default everywhere.

---

## SHARED-FROM-DAY-ONE SUBSYSTEMS

A key design decision is that some code should be treated as shared from the outset rather than first written as daemon-private glue.

### TLS

A shared TLS subsystem does **not** exist yet in the tree. If HTTPS is implemented, the TLS logic should live in a shared subsystem from day one, for example under `src/shared/tls/`.

That shared subsystem should present a narrow API and own:

- record parsing and emission
- handshake state
- key schedule support built on HKDF
- certificate and key loading hooks
- alert handling
- cipher-suite selection

Most importantly, the TLS API must be compatible with the default event-loop model:

- the TLS subsystem performs **no direct socket I/O**
- it consumes input bytes provided by the daemon
- it produces output bytes for the daemon to flush
- it reports whether it needs more input, has output pending, or has completed a state transition

In other words, the daemon owns the file descriptor and the connection buffers; the TLS layer owns only TLS state. This property is required so the event loop does not collapse into accidental blocking behavior.

### HTTP

A shared HTTP helper layer does **not** exist yet either. If both a future `httpsd` and other HTTP-capable tools need common request or header parsing, the reusable pieces belong in a small shared HTTP helper area rather than being copied across daemons.

### Accounts and identity

On hosted POSIX and macOS builds, user and group lookup already exist through the current identity backends. On the freestanding Linux side, identity handling is currently much more minimal. A server-oriented shared account layer does not yet exist, so if the system wants SSH login, IRC authentication, or privilege dropping beyond the hosted environment, that substrate still needs to be designed and implemented.

### Logging

A shared server logging subsystem does not yet exist. Log escaping is a cross-daemon security property and should not be reimplemented independently inside each `*_log.c` file. A small shared logging layer under `src/shared/` is justified from day one if the first daemons are added, so that:

- untrusted input is escaped consistently
- log injection handling is uniform
- noisy-client rate limiting can be shared
- supervisor log redirection and daemon logging use the same rules

---

## THREAT MODEL

The primary adversary for public-facing daemons is an untrusted remote network client. “Strict parsing” is necessary but not sufficient.

Each daemon should therefore explicitly defend against at least:

- slowloris-style connection starvation
- slow request body or slow read attacks
- oversized headers, lines, packets, or path inputs
- ambiguous HTTP message framing and request-smuggling vectors, including conflicting, duplicated, or inconsistent `Content-Length` / `Transfer-Encoding` handling
- path traversal and file-serving escape attempts
- timing differences in password, MAC, or token comparison
- log injection through untrusted input
- resource exhaustion by connection count, request count, or invalid handshake floods

### HTTPS-specific concerns

For HTTP and HTTPS daemons, the initial design should explicitly assume that the first version will **not** implement the whole web platform. That is acceptable, but the subset must be stated clearly.

A safe first subset is:

- `GET` and `HEAD` only
- no CGI
- no directory listing by default
- no symlink escape outside the configured document root
- no range requests in the first version
- no conditional request complexity beyond a very small subset, or none at all initially
- reject unsupported or ambiguous request framing rather than attempting permissive recovery

### SSH-specific concerns

For SSH, the design should explicitly account for:

- strict packet-length checks
- rekey and transport-state correctness
- key separation and host-key management
- privilege separation between the network-facing side and the session side

A separate `threat-model(7)` page would still be worthwhile later, but the minimum assumptions should already be written here.

---

## ACCOUNT AND AUTHENTICATION MODEL

This project cannot assume PAM, NSS, or a full `/etc/shadow`-style hosted environment.

Therefore the initial server design should make one of these explicit choices:

1. a small repository-local account database and key database,
2. key-only SSH authentication for a controlled single-operator system,
3. a single fixed login identity for early bring-up.

The recommended path for early implementation is:

- support **public-key authentication first**,
- keep password authentication optional and later,
- store the initial authorization data in a small admin-owned repository-local file format with restrictive permissions,
- load that file at daemon startup and explicitly reload it only through a controlled restart or future reload mechanism.

If password authentication is added later, it should use a modern slow password hash rather than a legacy fast hash.

That same choice also informs service supervision and privilege dropping. “Drop privileges” only makes sense once the system can name the target identity concretely.

---

## CRYPTO BASELINE

The existing crypto tree is already ahead of a minimal baseline.

Code currently present under `src/shared/crypto/` includes at least:

- SHA-256
- SHA-512
- HMAC-SHA256
- HKDF-SHA256
- RSA
- Curve25519 / X25519
- Ed25519
- ChaCha20-Poly1305
- SSH-specific KDF support

What is still missing is not “crypto exists at all”, but rather:

- full server-facing TLS subsystem structure
- any still-needed cipher or compatibility primitives such as AES-GCM if the chosen interoperability target requires them
- consistent build wiring so every primitive that should be generally reusable is actually included where needed
- broader protocol-level verification and interop testing

RSA support remains useful for compatibility and existing key material, but it should not define the long-term direction on its own.

For all such primitives, passing test vectors is necessary but not sufficient. The intended property for secret-dependent operations is constant-time behavior with respect to attacker-controlled input wherever the primitive requires it. This should be treated as part of the design contract, not as an optional later optimization.

---

## PLATFORM INTERFACE EXPECTATIONS

The current `platform.h` boundary is the correct place for OS interaction, and it already provides useful server-adjacent pieces today, including TCP connect, polling, DNS, DHCP, ping, process spawning, waiting, environment handling, and basic file operations across the hosted POSIX and freestanding Linux backends.

What serious server work will likely still require is some platform growth.

The first wave of additions may include:

- listener-specific helpers beyond the current client-oriented networking surface
- mandatory close-on-exec / non-inheritable descriptor handling for all newly created sockets and other sensitive file descriptors, using atomic creation flags where available and immediate fail-closed fallback where they are not
- explicit socket option helpers
- graceful shutdown support
- openat-style safe file access helpers for static serving and symlink policy enforcement
- possibly a clearer readiness / polling abstraction for many connections

This document intentionally does **not** promise every Linux-specific high-performance optimization from day one. Correctness, bounded behavior, and cross-build consistency come first. Linux-specific fast paths can be added later where clearly justified.

That is a deliberate trade-off, not an accident.

---

## HTTPS SERVER

### Recommended placement

The repository now contains a plain hosted `httpd` that follows the same ownership and layering rules for a version-one static server. A dedicated HTTPS daemon should extend that shape and dock into the tree roughly like this:

- `src/tools/httpsd.c` - public entry point, CLI, config loading, startup
- `src/tools/httpsd/httpsd_main.c` - listener setup and main event loop
- `src/tools/httpsd/https_listener.c` - bind, listen, accept, connection admission
- `src/tools/httpsd/https_conn.c` - per-connection buffers and lifetime management
- `src/tools/httpsd/http_parse.c` - strict request and header parsing
- `src/tools/httpsd/http_route.c` - fixed route dispatch
- `src/tools/httpsd/http_static.c` - tightly scoped static-file serving
- `src/tools/httpsd/https_log.c` - local access/error logging

TLS should dock into shared code under `src/shared/tls/` from the beginning, as described in the shared TLS section above, rather than remaining private inside the daemon.

### Safety and scope

A first HTTPS daemon should aim for a narrow, secure subset rather than pretending to implement every web-server feature.

A realistic version-one scope is:

- one document root or a small fixed route table
- GET and HEAD only
- simple response building
- hard connection and request limits
- straightforward logging
- no server-side scripting or plugin system

The “no symlink escape outside the configured document root” rule should be enforced by mechanism, not by intention alone. The preferred path is safe descriptor-relative opening via the platform layer, and where needed privilege dropping or jail-style filesystem restriction after bind and before the steady-state accept loop.

This is a good place to push back against feature creep: small scope is a feature here, not a deficiency.

---

## SSH SERVER

### Recommended placement

The repository already contains an SSH client organized as:

- `src/tools/ssh.c`
- `src/tools/ssh/ssh_core.c`
- `src/tools/ssh/ssh_client_io.c`
- `src/tools/ssh/ssh_client_kex.c`
- `src/tools/ssh/ssh_client_auth.c`
- `src/tools/ssh/ssh_client_channel.c`

An SSH server should follow the same ownership rule, but as a separate daemon:

- `src/tools/sshd.c`
- `src/tools/sshd/sshd_main.c`
- `src/tools/sshd/sshd_transport.c`
- `src/tools/sshd/sshd_kex.c`
- `src/tools/sshd/sshd_auth.c`
- `src/tools/sshd/sshd_channel.c`
- `src/tools/sshd/sshd_keys.c`
- optionally `src/tools/sshd/sshd_pty.c`

### Design commitment

This daemon should explicitly favor:

- small network-facing control path
- strict auth boundary
- session isolation after authentication
- key-first authentication strategy in early versions

The full privilege-separation model should be understood as a monitor protocol, not merely as a fork. In the stronger design:

- a small privileged monitor owns host keys, account lookups, and sensitive file access
- the unprivileged network child handles untrusted wire traffic
- requests such as public-key authorization checks are passed over a narrow internal control channel

If the first SSH daemon version cannot yet support that full monitor design safely, it should explicitly scope itself as a simpler single-identity or reduced-feature server rather than pretending that a late privilege drop is equivalent.

If shared wire-format or key-parsing helpers become common between client and server, they may move to `src/shared/`, but only once the boundary is clear.

---

## IRC, NNTP, AND OTHER SMALL TEXT PROTOCOLS

These are excellent fits for the project philosophy.

Suggested layout:

- `src/tools/ircd.c` with private files under `src/tools/ircd/`
- `src/tools/nntpd.c` with private files under `src/tools/nntpd/`

They work well with the default event-loop model and reward:

- bounded line buffers
- direct parser code
- explicit user / session / channel or spool state
- small, protocol-specific implementations

If IRC authentication or operator control is implemented, it should reuse the same shared account or key substrate described above rather than inventing a completely separate daemon-local credential store.

These are much better near-term targets than full federation-heavy social protocols.

---

## MATRIX, MASTODON, AND FEDERATED SYSTEMS

This document explicitly scopes these as **later and much larger** efforts.

Although they sit on HTTP or HTTPS, they also require outbound client behavior, durable queues, retry logic, signing, persistent storage, and federation interoperability. They should therefore **not** be thought of as “just `httpsd` plus a handler module”.

If the project moves in that direction later, it would benefit from a separate `federation(7)` design note.

This is a deliberate pushback against underestimating their size.

---

## SPECIALIZED HTTP API SERVICES

These are among the best matches for the current architecture.

Examples include:

- a metadata endpoint over files or archives
- a local administration endpoint
- a tiny JSON or plain-text query service
- a narrow internal storage or search service

These services can also reuse the shared account layer where authentication is needed, rather than inventing daemon-local credential formats.

Suggested layout:

- `src/tools/apid.c`
- `src/tools/apid/apid_main.c`
- `src/tools/apid/apid_parse.c`
- `src/tools/apid/apid_routes.c`
- `src/tools/apid/apid_store.c`

This intentionally favors fixed routes and small direct handlers over generic web-framework machinery.

---

## CONFIGURATION STORY

Configuration should not become a pile of unrelated mini-languages.

If multiple daemons are added, they should converge on one small declarative format shared across tools, using a repository-local parser rather than a large external dependency.

A simple key/value or sectioned format is preferable to a heavy framework-style configuration stack. If it proves reusable, the parser belongs in `src/shared/`.

### Hosted service tree convention

For actual hosted daemon instances, the repository now adopts a dedicated `services/` tree.
The convention is that the tool name determines the instance directory name.

For example, an HTTP daemon instance should live under:

- `services/httpd/www-root/` for the served static content such as `index.html`
- `services/httpd/config/` for daemon and supervisor config files
- `services/httpd/log/` for instance-local logs

Only deliberately public assets should live under `www-root/`. Configuration, pidfiles, logs, and any privilege-drop settings should remain outside that tree and must not be published from it.

The same pattern should apply to future daemons:

- `services/sshd/`
- `services/ircd/`
- `services/apid/`

and so on, with `www-root/` present only where the daemon actually serves a document tree.

This keeps deployment-facing assets out of `src/` while preserving the ownership rule that implementation code still lives under `src/tools/<tool>/`.

---

## SERVICE SUPERVISION

Start / stop / restart control still belongs in userland, not in a full init replacement.

The current tree already provides related building blocks in:

- `src/tools/init.c`
- `src/tools/getty.c`
- `src/tools/shutdown.c`
- `src/platform/*/process.c`

A first small supervisor now docks as:

- `src/tools/service.c`
- with private logic under `src/tools/service/`

It currently provides the version-one responsibilities described below and remains intentionally small in scope.

### Version-one responsibilities

A realistic first version can own:

- pid tracking
- start / stop / restart / status
- signal-based shutdown
- basic crash backoff
- log redirection
- privilege-drop configuration through service-managed user and group settings

### Explicitly deferred features

To avoid false expectations, the following are **not** assumed to exist immediately:

- full dependency ordering
- socket activation
- advanced readiness protocols
- cgroup-style resource management
- log rotation orchestration
- supervising the whole machine lifecycle
- certificate rotation without daemon restart or connection handoff
- general config-reload semantics for every daemon

That does not mean they are impossible; it means the first supervisor should remain small and honest about scope.

---

## MATCHING SKELETON PLAN

The following outlines match the current repository style and can be used as the initial shape for new daemon work.

### Minimal service supervisor

Public entry point:

- `src/tools/service.c` or `src/tools/sv.c`

Private layout:

- `src/tools/service/service_main.c` - command dispatch for `start`, `stop`, `restart`, `status`
- `src/tools/service/service_pidfile.c` - pidfile read/write and stale-pid checks
- `src/tools/service/service_spawn.c` - spawn, detach, log redirection, privilege-drop hooks
- `src/tools/service/service_signal.c` - graceful stop, reload, restart signaling
- `src/tools/service/service_config.c` - tiny shared-style config parsing

### HTTPS daemon skeleton

Public entry point:

- `src/tools/httpsd.c`

Private layout:

- `src/tools/httpsd/httpsd_main.c`
- `src/tools/httpsd/https_listener.c`
- `src/tools/httpsd/https_conn.c`
- `src/tools/httpsd/http_parse.c`
- `src/tools/httpsd/http_route.c`
- `src/tools/httpsd/http_static.c`
- `src/tools/httpsd/https_log.c`

Shared dependencies planned from the outset:

- `src/shared/tls/` for TLS mechanics
- `src/shared/http/` if generic request / header parsing becomes shared enough to justify it
- `src/shared/log/` for escaping and common logging behavior

### SSH daemon skeleton

Public entry point:

- `src/tools/sshd.c`

Private layout:

- `src/tools/sshd/sshd_main.c`
- `src/tools/sshd/sshd_transport.c`
- `src/tools/sshd/sshd_kex.c`
- `src/tools/sshd/sshd_auth.c`
- `src/tools/sshd/sshd_channel.c`
- `src/tools/sshd/sshd_keys.c`
- optionally `src/tools/sshd/sshd_pty.c`

### IRC daemon skeleton

Public entry point:

- `src/tools/ircd.c`

Private layout:

- `src/tools/ircd/ircd_main.c`
- `src/tools/ircd/irc_parse.c`
- `src/tools/ircd/irc_session.c`
- `src/tools/ircd/irc_channel.c`
- `src/tools/ircd/irc_reply.c`

### Specialized API daemon skeleton

Public entry point:

- `src/tools/apid.c`

Private layout:

- `src/tools/apid/apid_main.c`
- `src/tools/apid/apid_parse.c`
- `src/tools/apid/apid_routes.c`
- `src/tools/apid/apid_store.c`

---

## REALISTIC ROADMAP

The implementation order in the current repository should be:

1. extend `platform.h` and `src/platform/*` only where server work genuinely requires it
2. decide the account / identity story
3. add a small shared logging and config foundation if multiple daemons are expected
4. add a small service supervisor with explicit deferred scope
5. add `sshd` with a clear auth and session-isolation model
6. add shared TLS support as a first-class subsystem
7. budget the crypto engineering needed for modern TLS / SSH primitives and verification
8. build `httpsd` on top of that shared TLS layer
9. only after real reuse emerges, extract further shared HTTP or daemon helpers

Testing and observability should be treated as deferred only in the sense of staging, not in the sense of being optional. Parser fuzzing, crypto vector coverage, protocol interop checks, and simple runtime counters are all part of making these daemons trustworthy.

This should be treated as substantial engineering work, not as a weekend feature list. The document intentionally prefers honest sequencing over optimistic shorthand.

## SEE ALSO

man, project-layout, platform, runtime, build, userland
