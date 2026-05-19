# MKNOD

## NAME

mknod - create FIFOs and device nodes

## SYNOPSIS

```
mknod [-m MODE] NAME TYPE [MAJOR MINOR]
```

## DESCRIPTION

`mknod` creates a filesystem node. FIFOs can usually be created by an ordinary
user in writable directories. Character and block devices normally require
elevated privileges and a filesystem that permits device nodes.

## OPTIONS

- `-m MODE` - set octal permission bits; the default is `666`
- `-h`, `--help` - show usage information

## OPERANDS

- `NAME` - path to create
- `TYPE` - `p` or `fifo` for a FIFO, `c`, `u`, or `char` for a character
  device, and `b` or `block` for a block device
- `MAJOR MINOR` - device numbers required for character and block devices

## LIMITATIONS

- Symbolic mode strings are not accepted for `-m`; use octal permissions.
- No `--reference`, SELinux context, ACL, or extended-attribute support is
  implemented.
- Device creation depends on kernel, filesystem, container, and privilege
  policy; the tool does not virtualize or emulate missing device-node support.
- It does not consult `/proc/devices` or validate whether a major/minor pair
  corresponds to a real driver.

## EXAMPLES

```
mknod -m 600 /tmp/control p
mknod /dev/console c 5 1
mknod /dev/sda b 8 0
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

mkdir, chmod, stat
