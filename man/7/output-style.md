# OUTPUT-STYLE

## NAME

output-style - shared terminal color and emphasis behavior for newos tools

## DESCRIPTION

Several interactive tools in the project can now use a shared styling layer from
`src/shared` to emit ANSI terminal colors and bold text when the output stream
supports it. This keeps color behavior consistent across tools such as `ls`,
`grep`, `diff`, and `make`.

The shared policy is:

- use colors automatically when writing to a terminal
- suppress colors for non-tty output unless explicitly forced
- allow users to disable or force styling centrally with common environment
  variables and per-tool `--color=WHEN` flags

## COLOR MODES

- `--color=auto` - enable styling only for tty output
- `--color=always` - always emit ANSI styling sequences
- `--color=never` - disable styling entirely

## ENVIRONMENT

- `NO_COLOR` - disables color output in auto mode
- `CLICOLOR=0` - disables color output in auto mode
- `CLICOLOR_FORCE=1` - forces color output even when not writing to a tty
- `TERM=dumb` - suppresses color output in auto mode

## NOTES

- Tools still aim to remain readable when colors are disabled.
- Styling is intentionally lightweight: bold/bright emphasis and common
  foreground colors rather than a full theming system.
- Non-interactive uses such as scripts or redirected output should normally use
  plain text unless color is explicitly forced.

## SEE ALSO

ls, grep, diff, make, sh
