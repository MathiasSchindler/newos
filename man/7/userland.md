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
- filesystem and paths: `ls`, `cp`, `mv`, `rm`, `mkdir`, `ln`, `chmod`,
  `chown`, `stat`, `du`, `df`, `touch`, `realpath`, `readlink`
- text and streams: `cat`, `grep`, `sed`, `awk`, `sort`, `cut`, `tr`, `wc`,
  `head`, `tail`, `join`, `split`, `xargs`, `tee`, `fmt`, `column`
- system and reporting: `ps`, `pstree`, `free`, `uptime`, `who`, `users`,
  `groups`, `id`, `hostname`, `uname`, `date`, `sleep`, `watch`
- network and transfer: `ping`, `netcat`, `ssh`, `wget`
- archive, patching, and build: `tar`, `gzip`, `gunzip`, `patch`, `make`, `ncc`

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

## HIGH-VALUE REMAINING GAPS

For a fuller self-hosted or stand-alone OS environment, the most valuable
missing tools are now concentrated in system bringup and administration:

- `init` or a tiny early-userspace launcher
- `mount` and `umount`
- `dmesg`
- a small `ip` tool for links, addresses, and routes
- `getty` and possibly `login`
- a DHCP client and a small DNS lookup tool

Those are now more strategically important than adding yet another text filter,
because they unlock more of the "boot into the project and live there" story.

## BRING-UP PRIORITIES

If the goal is to move toward a more self-sufficient operating environment, a
practical order is:

1. `mount`, `umount`, and a small `init`
2. `dmesg`, `getty`, and console/session plumbing
3. `ip` plus DHCP/DNS basics
4. later account-management and multi-user tools such as `login`, `passwd`, and
   `su`

## LIMITATIONS

- Many existing tools still intentionally implement the practical subset rather
  than full POSIX or GNU parity
- The system-management layer is thinner than the text/filter/file tool layer
- Networking, packaging, and boot orchestration remain less complete than the
  day-to-day development userland

## SEE ALSO

project-layout, platform, build, testing, shell, compiler
