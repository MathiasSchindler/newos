# MOUNT

## NAME

mount - list mounted file systems or attach a file system at a target path

## SYNOPSIS

```sh
mount
mount [-rvwB] [-t TYPE] [-o OPTIONS] SOURCE TARGET
```

## DESCRIPTION

With no positional arguments, `mount` prints the current kernel mount table when
that information is available (currently via `/proc/self/mounts` or
`/proc/mounts` on Linux-style systems).

With `SOURCE` and `TARGET`, it asks the kernel to mount a file system. This is a
small Linux-first implementation intended for early userspace and simple system
setup tasks.

## CURRENT CAPABILITIES

- list current mounts on Linux-style systems
- mount a file system with an explicit type via `-t`
- pass common mount flags such as read-only, bind, remount, and noexec
- forward additional filesystem-specific options via `-o`

## OPTIONS

- `-t TYPE`, `--types TYPE` - set the filesystem type such as `proc`, `tmpfs`,
  `ext4`, or `sysfs`
- `-o OPTIONS`, `--options OPTIONS` - comma-separated mount options; recognized
  flags include `ro`, `rw`, `bind`, `remount`, `nosuid`, `nodev`, `noexec`,
  `sync`, `dirsync`, `relatime`, `noatime`, `nodiratime`, `lazytime`, and
  `silent`
- `-r`, `--read-only` - mount read-only
- `-w`, `--read-write` - clear read-only mode
- `-B`, `--bind` - request a bind mount
- `-v`, `--verbose` - print a short success message after mounting
- `-h`, `--help` - show usage information

## LIMITATIONS

- mounting requires appropriate kernel privileges
- current full mount operations are implemented for Linux; non-Linux hosted
  platforms may compile the tool but not support real mounts
- no `/etc/fstab` parsing, `-a`, label/UUID lookup, or automatic filesystem
  probing yet
- mount listing currently prints the kernel table directly instead of formatting
  a richer column view

## EXAMPLES

```sh
mount
mount -t proc proc /proc
mount -t tmpfs -o ro,size=16m tmpfs /mnt
mount -B /oldroot /newroot
```

## SEE ALSO

umount, df, mkdir, init
