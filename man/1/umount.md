# UMOUNT

## NAME

umount - detach mounted file systems

## SYNOPSIS

```sh
umount [-flv] TARGET...
```

## DESCRIPTION

`umount` asks the kernel to detach one or more mounted file systems from their
target paths. It is a small Linux-first counterpart to the project's `mount`
tool and is aimed at direct system or early-userspace use.

## CURRENT CAPABILITIES

- unmount one or more targets in one invocation
- request forced unmounts with `-f`
- request lazy detaches with `-l`
- print per-target confirmation with `-v`

## OPTIONS

- `-f`, `--force` - request a forced unmount when the platform supports it
- `-l`, `--lazy` - detach lazily when the platform supports it
- `-v`, `--verbose` - print each successful unmount target
- `-h`, `--help` - show usage information

## LIMITATIONS

- requires appropriate privileges
- does not currently support `-a`, recursive unmount sets, or `/etc/fstab`
  driven behavior
- lazy unmount support is Linux-oriented; other hosted platforms may reject it

## EXAMPLES

```sh
umount /mnt
umount -v /mnt/data
umount -l /oldroot
```

## SEE ALSO

mount, df, sync
