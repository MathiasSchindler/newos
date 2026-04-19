# shutdown

Request an immediate system power state change.

## Synopsis

shutdown [options] [now]

## Options

- `-r`, `--reboot` reboot the system (default)
- `-p`, `-P`, `--poweroff` request power-off
- `-H`, `-h`, `--halt` halt the system without requesting power-off
- `--help` show help text

Only immediate operation is currently supported, so use forms such as:

- `shutdown now`
- `shutdown -r now`
- `shutdown -h now`

In the experimental initramfs guest, `shutdown now` is intended to end the QEMU session cleanly by requesting an immediate reboot, which QEMU exits from under the current launcher settings.
