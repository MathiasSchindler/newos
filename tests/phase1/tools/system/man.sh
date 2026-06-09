#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup man

"${TEST_BIN_DIR}/man" ls > "$WORK_DIR/man_ls.out"
assert_file_contains "$WORK_DIR/man_ls.out" '^LS$' "man did not open the ls manual page"
assert_file_contains "$WORK_DIR/man_ls.out" 'list files and directories' "man page content for ls was missing"

"${TEST_BIN_DIR}/man" 7 project-layout > "$WORK_DIR/man_layout.out"
assert_file_contains "$WORK_DIR/man_layout.out" 'overview of the repository structure' "man section lookup failed"

"${TEST_BIN_DIR}/man" 7 unicode > "$WORK_DIR/man_unicode.out"
assert_file_contains "$WORK_DIR/man_unicode.out" 'implementation roadmap for full Unicode support' "man did not open the unicode design page"

"${TEST_BIN_DIR}/man" env > "$WORK_DIR/man_env.out"
assert_file_contains "$WORK_DIR/man_env.out" '^ENV$' "man did not open the env manual page"
assert_file_contains "$WORK_DIR/man_env.out" 'emit NUL-delimited output with -0' "man output missed env option details"

"${TEST_BIN_DIR}/man" touch > "$WORK_DIR/man_touch.out"
assert_file_contains "$WORK_DIR/man_touch.out" 'GNU-style long aliases' "man output missed documented touch long-option support"
assert_file_contains "$WORK_DIR/man_touch.out" 'access/atime/use or modify/mtime' "man output missed touch time-selector details"

"${TEST_BIN_DIR}/man" sync > "$WORK_DIR/man_sync.out"
assert_file_contains "$WORK_DIR/man_sync.out" 'single confirmation when syncing all filesystems' "man output missed sync global verbose behavior"

"${TEST_BIN_DIR}/man" -k compiler > "$WORK_DIR/man_search.out"
assert_file_contains "$WORK_DIR/man_search.out" '^ncc (1)$' "man -k did not find the compiler page"

"${TEST_BIN_DIR}/man" --json -k compiler > "$WORK_DIR/man_search_json.out"
assert_file_contains "$WORK_DIR/man_search_json.out" '"schema":"newos.tool.v1"' "man --json -k did not use the shared JSON envelope"
assert_file_contains "$WORK_DIR/man_search_json.out" '"event":"man_search_result"' "man --json -k did not emit search result events"
assert_file_contains "$WORK_DIR/man_search_json.out" '"name":"ncc"' "man --json -k did not report the matched page name"

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
"${TEST_BIN_DIR}/man" -l "$WORK_DIR/man_render.md" > "$WORK_DIR/man_render.out"
assert_file_contains "$WORK_DIR/man_render.out" '^  [|] quoted note$' "man did not render block quotes cleanly"
assert_file_contains "$WORK_DIR/man_render.out" '^    echo hi$' "man did not preserve fenced code blocks as indented text"
assert_file_contains "$WORK_DIR/man_render.out" '^Literal flag: --color\[=WHEN\]$' "man did not preserve literal bracketed option syntax"
assert_file_contains "$WORK_DIR/man_render.out" '^┌.*┐$' "man did not render markdown tables with Unicode borders"
assert_file_contains "$WORK_DIR/man_render.out" '^│ Flag .* │$' "man did not render the table header row cleanly"

"${TEST_BIN_DIR}/man" --json -l "$WORK_DIR/man_render.md" > "$WORK_DIR/man_render_json.out"
assert_file_contains "$WORK_DIR/man_render_json.out" '"event":"man_page_start"' "man --json -l did not emit man_page_start"
assert_file_contains "$WORK_DIR/man_render_json.out" '"event":"man_page_chunk"' "man --json -l did not emit man_page_chunk"
assert_file_contains "$WORK_DIR/man_render_json.out" '"markdown":"# RENDER' "man --json -l did not include raw markdown content"
assert_file_contains "$WORK_DIR/man_render_json.out" '"event":"man_page_complete"' "man --json -l did not emit man_page_complete"

cat > "$WORK_DIR/man_wrap.md" <<'EOF'
# WRAP

| Col1 | Col2 |
|------|------|
| Value 1 Separate | Value 2 cols |
| This is a row with only one cell |
EOF
COLUMNS=28 "${TEST_BIN_DIR}/man" -l "$WORK_DIR/man_wrap.md" > "$WORK_DIR/man_wrap.out"
assert_file_contains "$WORK_DIR/man_wrap.out" '^┌.*┐$' "man did not keep the wrapped table framed"
assert_file_contains "$WORK_DIR/man_wrap.out" 'Separate' "man did not wrap long first-column content"
assert_file_contains "$WORK_DIR/man_wrap.out" 'cols' "man did not wrap long second-column content"
assert_file_contains "$WORK_DIR/man_wrap.out" 'only one' "man did not keep a single-cell row visible after wrapping"

cat > "$WORK_DIR/man_unicode_width.md" <<'EOF'
# WIDTH

| Word | Meaning |
|------|---------|
| écho | combining mark |
| 界界 | wide glyphs |

wide: 界界abcdefghi
EOF
COLUMNS=16 "${TEST_BIN_DIR}/man" -l "$WORK_DIR/man_unicode_width.md" > "$WORK_DIR/man_unicode_width.out"
assert_file_contains "$WORK_DIR/man_unicode_width.out" 'écho' "man table rendering dropped combining-mark text"
assert_file_contains "$WORK_DIR/man_unicode_width.out" '界界' "man table rendering dropped wide-character text"
assert_file_contains "$WORK_DIR/man_unicode_width.out" '^wide: 界界abcdef$' "man wrapping did not account for wide Unicode columns"
assert_file_contains "$WORK_DIR/man_unicode_width.out" '^ghi$' "man wrapping did not continue after a wide Unicode line break"

"${TEST_BIN_DIR}/man" --color=always -l "$WORK_DIR/man_render.md" > "$WORK_DIR/man_color.out"
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
MANPATH="$WORK_DIR/manroot" "${TEST_BIN_DIR}/man" -k depthsentinel > "$WORK_DIR/man_deep_search.out"
assert_file_contains "$WORK_DIR/man_deep_search.out" '^deep (1)$' "man -k only searched an initial prefix of the page"

"${TEST_BIN_DIR}/man" -k 'überblick' > "$WORK_DIR/man_unicode_search.out"
assert_file_contains "$WORK_DIR/man_unicode_search.out" '^unicode (7)$' "man keyword search did not find the unicode design page"
