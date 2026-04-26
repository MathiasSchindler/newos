# PING6

## NAME

ping6 - IPv6 echo-request compatibility alias for ping

## SYNOPSIS

```
ping6 [PING OPTIONS] HOST
```

## DESCRIPTION

`ping6` is a small compatibility entry point for IPv6 probing. It behaves like
running `ping -6` and accepts the same options.

## CURRENT CAPABILITIES

- force IPv6 probing by default
- share the same timeout, count, size, and deadline handling as `ping`
- work as a convenience alias in scripts and interactive shells

## LIMITATIONS

- capability and privilege requirements are the same as for `ping -6`
- this is an alias-style entry point, not a separate implementation

## EXAMPLES

```
ping6 ::1
ping6 -c 3 host.example
```

## SEE ALSO

ping
