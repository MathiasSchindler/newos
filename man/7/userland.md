# USERLAND

## NAME

userland - current command surface and system-bringup priorities

## DESCRIPTION

The repository already contains a broad small-Unix-style userland under
`src/tools/`. The goal is not to clone every corner of GNU or BusyBox, but to
provide a practical, self-contained environment that stays useful in both the
hosted development build and the freestanding Linux target.

This page summarizes the current base and the highest-value missing pieces for a
more complete stand-alone system.

## CURRENT BASE

The project already has working coverage across several major tool families:

- shell and scripting: `sh`, `test`, the bracket alias for `test`, `env`,
  `printenv`, and `timeout`
- filesystem and paths: `ls`, `cp`, `mv`, `rm`, `mkdir`, `mknod`, `ln`,
  `chmod`, `chown`, `stat`, `du`, `df`, `touch`, `realpath`, `readlink`
- text and streams: `cat`, `grep`, `sed`, `awk`, `sort`, `cut`, `tr`, `wc`,
  `head`, `tail`, `join`, `split`, `xargs`, `tee`, `fmt`, `column`
- system and reporting: `init`, `getty`, `login`, `dmesg`, `logger`, `stty`,
  `ps`, `pstree`, `free`, `uptime`, `who`, `users`, `groups`, `id`,
  `hostname`, `uname`, `date`, `sleep`, `watch`
- network and transfer: `ping`, `ping6`, `ip`, `dhcp`, `nslookup`, `dig`,
  `netcat`, `ssh`, `wget`, `httpd`, `service`
- archive, patching, and build: `tar`, `gzip`, `gunzip`, `patch`, `make`, `ncc`
- media metadata: `file` and `imginfo` for lightweight image/file inspection

That means the repo is already well beyond a bootstrap shell and a handful of
toy commands: it has enough surface for real development, testing, and tool
expansion inside the project itself.

## DESIGN RULES

When adding or extending userland tools:

- prefer statically linked, freestanding-friendly implementations whenever
  practical
- keep one public entry point per tool in `src/tools/`
- if a tool grows internal modules, place them in `src/tools/<tool>/`
- keep only genuinely reusable cross-tool behavior in `src/shared/`
- route OS interaction through the shared platform interface instead of pulling
  host APIs directly into generic tool code
- optimize for useful real-world behavior before chasing obscure compatibility
  edge cases

## SHARED PRIMITIVES

The project maintains shared implementation primitives in `src/shared/` that
multiple tools can reuse:

- **bignum.{c,h}** - freestanding arbitrary-precision signed integer arithmetic,
  used by `expr` and `bc` to provide high-range numeric support without libc or
  heap allocation. The implementation uses base-1000000000 representation with
  up to 128 digits (approximately 1150 decimal digits capacity) and supports
  addition, subtraction, multiplication, division, modulo, comparisons, and
  power/scale operations.

- **platform.h** - OS abstraction for syscalls and platform-specific features

- **runtime.{c,h}** - freestanding runtime helpers (string, memory, I/O) that
  work without libc

- **tool_util.h** - common tool argument parsing and error reporting

- **image/image.{c,h}** - safe metadata probing for common image containers,
  currently used by `imginfo` without decoding pixels or allocating image-sized
  buffers.

Other shared components include archive utilities, crypto, hash functions,
regex, path manipulation, and shell infrastructure.

## HIGH-VALUE REMAINING GAPS

For a fuller self-hosted or stand-alone OS environment, the most valuable
missing tools are now concentrated in system bringup and administration:

- account-management tools such as `passwd`, `su`, and shadow-file helpers
- richer service supervision and boot orchestration around `init` and `service`
- deeper DHCP renewal, IPv6 address changes, and network autoconfiguration
- package/archive maintenance tools beyond the current tar/gzip/patch base
- auditing and diagnostics around permissions, sessions, and startup logs

Those are now more strategically important than adding yet another text filter,
because they unlock more of the "boot into the project and live there" story.

## BRING-UP PRIORITIES

If the goal is to move toward a more self-sufficient operating environment, a
practical order is:

1. add password/account management around the new `login` helper
2. deepen console/session tracking so `who` and `login` can share records
3. improve service supervision, restart policy, and boot orchestration
4. extend networking with DHCP renewal, IPv6 mutation, and richer diagnostics

## LIMITATIONS

- Many existing tools still intentionally implement the practical subset rather than full POSIX or GNU parity
- The system-management layer is thinner than the text/filter/file tool layer
- Networking, packaging, and boot orchestration remain less complete than the day-to-day development userland

## SEE ALSO

project-layout, platform, build, testing, shell, compiler
