# UNAME

## NAME

uname - print system and kernel identity information

## SYNOPSIS

```text
uname [-asnrvmipo] [--all] [--kernel-name] [--nodename] [--kernel-release] [--kernel-version] [--machine] [--processor] [--hardware-platform] [--operating-system]
```

## DESCRIPTION

`uname` prints identifying information about the current system through the
project's platform abstraction layer.

## CURRENT CAPABILITIES

- kernel name, hostname, release, version, and machine output
- default output of the kernel name when no flags are given
- aggregate output with `-a` or `--all`
- long-option forms for the common identity fields
- compatibility fields for processor, hardware platform, and operating-system
  style output

## OPTIONS

- `-a`, `--all` print the standard full summary
- `-s`, `-n`, `-r`, `-v`, `-m` select individual fields
- `-p`, `-i` request processor and hardware-platform style output; in the
  current implementation they map to the same machine string as `-m`
- `-o` requests operating-system style output; it currently maps to the project
  OS/kernel name

## LIMITATIONS

- some secondary fields intentionally map to the same underlying platform
  information (`-m`, `-p`, and `-i` may match)
- output reflects the current platform backend rather than host `uname` quirks

## EXAMPLES

```text
uname
uname -a
uname -sm
```

## SEE ALSO

hostname, id, uptime
