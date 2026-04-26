#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir ripgrep)

note "phase1 text: ripgrep/rg"

mkdir -p "$WORK_DIR/tree/src" "$WORK_DIR/tree/docs" "$WORK_DIR/tree/.hidden"
cat > "$WORK_DIR/tree/src/main.c" <<'EOF'
int main(void) {
    return platform_value;
}
EOF
cat > "$WORK_DIR/tree/src/helper.c" <<'EOF'
int helper(void) {
    return 42;
}
EOF
cat > "$WORK_DIR/tree/docs/notes.md" <<'EOF'
# Notes

platform value appears here too.
EOF
printf 'platform hidden\n' > "$WORK_DIR/tree/.hidden/secret.c"
printf 'binary\0platform\n' > "$WORK_DIR/tree/src/blob.bin"

"$ROOT_DIR/build/rg" platform "$WORK_DIR/tree" > "$WORK_DIR/basic.out"
assert_file_contains "$WORK_DIR/basic.out" 'src/main\.c:2:    return platform_value;' "rg did not report the source match with a line number"
assert_file_contains "$WORK_DIR/basic.out" 'docs/notes\.md:3:platform value appears here too\.' "rg did not search recursively into docs"
if grep -q 'secret\.c\|blob\.bin' "$WORK_DIR/basic.out"; then
    fail "rg should skip hidden files and binary files in normal search"
fi

"$ROOT_DIR/build/rg" -i -t md platform "$WORK_DIR/tree" > "$WORK_DIR/type.out"
assert_file_contains "$WORK_DIR/type.out" 'docs/notes\.md:3:platform value appears here too\.' "rg -i -t md did not find the Markdown match"
if grep -q 'main\.c' "$WORK_DIR/type.out"; then
    fail "rg -t md should not include C source matches"
fi

"$ROOT_DIR/build/rg" --hidden platform "$WORK_DIR/tree" > "$WORK_DIR/hidden.out"
assert_file_contains "$WORK_DIR/hidden.out" '\.hidden/secret\.c:1:platform hidden' "rg --hidden did not include hidden files"

"$ROOT_DIR/build/rg" --files -g '*.c' "$WORK_DIR/tree" > "$WORK_DIR/files.out"
assert_file_contains "$WORK_DIR/files.out" 'src/main\.c$' "rg --files -g missed main.c"
assert_file_contains "$WORK_DIR/files.out" 'src/helper\.c$' "rg --files -g missed helper.c"
if grep -q 'secret\.c\|notes\.md\|blob\.bin' "$WORK_DIR/files.out"; then
    fail "rg --files -g should respect hidden-file skipping and glob filtering"
fi

"$ROOT_DIR/build/ripgrep" --no-line-number platform "$WORK_DIR/tree/src/main.c" > "$WORK_DIR/no_line_number.out"
printf '    return platform_value;\n' > "$WORK_DIR/no_line_number.expected"
assert_files_equal "$WORK_DIR/no_line_number.expected" "$WORK_DIR/no_line_number.out" "ripgrep --no-line-number did not suppress line numbers"

"$ROOT_DIR/build/ripgrep" -F 'platform_value' "$WORK_DIR/tree/src/main.c" > "$WORK_DIR/fixed.out"
assert_file_contains "$WORK_DIR/fixed.out" '^2:    return platform_value;$' "ripgrep -F did not find the literal match"

assert_command_succeeds "$ROOT_DIR/build/rg" -q platform "$WORK_DIR/tree"
no_match_status=0
"$ROOT_DIR/build/rg" missing-pattern "$WORK_DIR/tree" > "$WORK_DIR/no_match.out" 2>&1 || no_match_status=$?
assert_text_equals "$no_match_status" '1' "rg should return 1 when no match is found"