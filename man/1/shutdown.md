# SHUTDOWN

Request an immediate system power state change.

## SYNOPSIS

```
shutdown [options] [now]
```

## OPTIONS

- `-r`, `--reboot` reboot the system (default)
- `-p`, `-P`, `--poweroff` request power-off
- `-H`, `-h`, `--halt` halt the system without requesting power-off
- `--help` show help text

Only immediate operation is currently supported, so use forms such as:

- `shutdown now`
- `shutdown -r now`
- `shutdown -h now`

In the experimental initramfs guest, `shutdown now` is intended to end the QEMU session cleanly by requesting an immediate reboot, which QEMU exits from under the current launcher settings.

## LIMITATIONS

- Only immediate shutdown forms are supported; scheduled times, `+MINUTES`,
  wall messages, cancellation, and warning broadcasts are not implemented.
- No integration with a full init/service manager exists yet, so ordered
  service stopping, user-session notification, and inhibitor locks are absent.
- Behavior depends on the platform backend and privileges; hosted environments
  may reject reboot, halt, or poweroff requests.
- No runlevel, single-user, kexec, or firmware-selection modes are provided.
