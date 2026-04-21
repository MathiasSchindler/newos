#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup man

"$ROOT_DIR/build/man" ls > "$WORK_DIR/man_ls.out"
assert_file_contains "$WORK_DIR/man_ls.out" '^LS$' "man did not open the ls manual page"
assert_file_contains "$WORK_DIR/man_ls.out" 'list files and directories' "man page content for ls was missing"

"$ROOT_DIR/build/man" 7 project-layout > "$WORK_DIR/man_layout.out"
assert_file_contains "$WORK_DIR/man_layout.out" 'overview of the repository structure' "man section lookup failed"

"$ROOT_DIR/build/man" 7 unicode > "$WORK_DIR/man_unicode.out"
assert_file_contains "$WORK_DIR/man_unicode.out" 'implementation roadmap for full Unicode support' "man did not open the unicode design page"

"$ROOT_DIR/build/man" env > "$WORK_DIR/man_env.out"
assert_file_contains "$WORK_DIR/man_env.out" '^ENV$' "man did not open the env manual page"
assert_file_contains "$WORK_DIR/man_env.out" 'emit NUL-delimited output with -0' "man output missed env option details"

"$ROOT_DIR/build/man" -k compiler > "$WORK_DIR/man_search.out"
assert_file_contains "$WORK_DIR/man_search.out" '^ncc (1)$' "man -k did not find the compiler page"

cat > "$WORK_DIR/man_render.md" <<'EOF'
# RENDER

> quoted note

- bullet entry

Literal flag: `--color[=WHEN]`

| Flag | Meaning |
|------|---------|
| `-a` | alpha |
| `-b` | beta mode |

```sh
echo hi
```
EOF
"$ROOT_DIR/build/man" -l "$WORK_DIR/man_render.md" > "$WORK_DIR/man_render.out"
assert_file_contains "$WORK_DIR/man_render.out" '^  [|] quoted note$' "man did not render block quotes cleanly"
assert_file_contains "$WORK_DIR/man_render.out" '^    echo hi$' "man did not preserve fenced code blocks as indented text"
assert_file_contains "$WORK_DIR/man_render.out" '^Literal flag: --color\[=WHEN\]$' "man did not preserve literal bracketed option syntax"
assert_file_contains "$WORK_DIR/man_render.out" '^┌.*┐$' "man did not render markdown tables with Unicode borders"
assert_file_contains "$WORK_DIR/man_render.out" '^│ Flag .* │$' "man did not render the table header row cleanly"

cat > "$WORK_DIR/man_wrap.md" <<'EOF'
# WRAP

| Col1 | Col2 |
|------|------|
| Value 1 Separate | Value 2 cols |
| This is a row with only one cell |
EOF
COLUMNS=28 "$ROOT_DIR/build/man" -l "$WORK_DIR/man_wrap.md" > "$WORK_DIR/man_wrap.out"
assert_file_contains "$WORK_DIR/man_wrap.out" '^┌.*┐$' "man did not keep the wrapped table framed"
assert_file_contains "$WORK_DIR/man_wrap.out" 'Separate' "man did not wrap long first-column content"
assert_file_contains "$WORK_DIR/man_wrap.out" 'cols' "man did not wrap long second-column content"
assert_file_contains "$WORK_DIR/man_wrap.out" 'only one' "man did not keep a single-cell row visible after wrapping"

"$ROOT_DIR/build/man" --color=always -l "$WORK_DIR/man_render.md" > "$WORK_DIR/man_color.out"
if ! LC_ALL=C grep -q "$(printf '\033')\\[" "$WORK_DIR/man_color.out"; then
    fail "man --color=always did not emit ANSI color sequences"
fi

mkdir -p "$WORK_DIR/manroot/1"
i=0
cat > "$WORK_DIR/manroot/1/deep.md" <<'EOF'
# DEEP

## NAME

deep - search depth coverage page

## DESCRIPTION
EOF
while [ "$i" -lt 180 ]; do
    printf 'Padding line %03d keeps this manual page well beyond four kibibytes of searchable content.\n' "$i" >> "$WORK_DIR/manroot/1/deep.md"
    i=$((i + 1))
done
printf '\nSpecial marker: depthsentinel.\n' >> "$WORK_DIR/manroot/1/deep.md"
MANPATH="$WORK_DIR/manroot" "$ROOT_DIR/build/man" -k depthsentinel > "$WORK_DIR/man_deep_search.out"
assert_file_contains "$WORK_DIR/man_deep_search.out" '^deep (1)$' "man -k only searched an initial prefix of the page"

"$ROOT_DIR/build/man" -k 'überblick' > "$WORK_DIR/man_unicode_search.out"
assert_file_contains "$WORK_DIR/man_unicode_search.out" '^unicode (7)$' "man keyword search did not find the unicode design page"
