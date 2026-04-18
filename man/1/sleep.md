# SLEEP

## NAME

sleep - delay execution

## SYNOPSIS

sleep DURATION...

## DESCRIPTION

The sleep tool pauses execution for the requested amount of time. Multiple duration arguments are added together before sleeping.

## CURRENT CAPABILITIES

- accept seconds by default
- support `ms`, `s`, `m`, `h`, and `d` suffixes
- accept fractional values such as `0.5s`
- sum multiple durations in one invocation

## OPTIONS

- `DURATION` — number with optional suffix `ms`, `s`, `m`, `h`, or `d`
- multiple duration arguments are summed before the delay starts

## LIMITATIONS

- The special values `inf` and `infinity` are not supported.
- Timing precision is limited to milliseconds.

## EXAMPLES

- `sleep 1`
- `sleep 250ms`
- `sleep 1m 30s`

## SEE ALSO

sh, env, kill
