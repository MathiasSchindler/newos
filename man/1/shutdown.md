# shutdown

Request an immediate system power state change.

## Synopsis

shutdown [options] [now]

## Options

- `-p`, `-P`, `--poweroff` power off the system (default)
- `-H`, `-h`, `--halt` halt the system without requesting power-off
- `-r`, `--reboot` reboot the system
- `--help` show help text

Only immediate operation is currently supported, so use forms such as:

- `shutdown now`
- `shutdown -r now`
- `shutdown -h now`

In the experimental initramfs guest, `shutdown now` can be used to end the QEMU session cleanly.
