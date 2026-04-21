#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup which

which_out=$(PATH="$ROOT_DIR/build:/bin:/usr/bin" "$ROOT_DIR/build/which" sh | tr -d '\r\n')
assert_text_equals "$which_out" "$ROOT_DIR/build/sh" "which did not resolve the in-repo binary first"

PATH="$ROOT_DIR/build:/bin:/usr/bin" "$ROOT_DIR/build/which" -a sh > "$WORK_DIR/which_all.out"
assert_file_contains "$WORK_DIR/which_all.out" "^$ROOT_DIR/build/sh$" "which -a did not include the in-repo binary"

which_builtin=$(PATH="$ROOT_DIR/build:/bin:/usr/bin" "$ROOT_DIR/build/which" cd | tr -d '\r\n')
assert_text_equals "$which_builtin" 'cd: shell built-in' "which should recognize shell built-ins"

which_function=$(env 'BASH_FUNC_phase1demo%%=() { :; }' PATH="$ROOT_DIR/build:/bin:/usr/bin" "$ROOT_DIR/build/which" phase1demo | tr -d '\r\n')
assert_text_equals "$which_function" 'phase1demo: shell function' "which should recognize exported shell functions"

mkdir -p "$WORK_DIR/path1" "$WORK_DIR/path2"
printf '#!/bin/sh\necho one\n' > "$WORK_DIR/path1/dupcmd"
printf '#!/bin/sh\necho two\n' > "$WORK_DIR/path2/dupcmd"
chmod +x "$WORK_DIR/path1/dupcmd" "$WORK_DIR/path2/dupcmd"
PATH="$WORK_DIR/path1:$WORK_DIR/path2:$PATH" "$ROOT_DIR/build/which" -a dupcmd > "$WORK_DIR/dupcmd_all.out"
assert_file_contains "$WORK_DIR/dupcmd_all.out" 'path1/dupcmd' "which -a missed the first PATH match"
assert_file_contains "$WORK_DIR/dupcmd_all.out" 'path2/dupcmd' "which -a missed the second PATH match"

missing_status=0
PATH="$ROOT_DIR/build:/bin:/usr/bin" "$ROOT_DIR/build/which" definitely_missing_command >/dev/null 2>&1 || missing_status=$?
assert_exit_code "$missing_status" '1' "which should fail for an unknown command"

malformed_status=0
env 'BASH_FUNC_phase1bad=' PATH="$ROOT_DIR/build:/bin:/usr/bin" "$ROOT_DIR/build/which" phase1bad >/dev/null 2>&1 || malformed_status=$?
assert_exit_code "$malformed_status" '1' "which should ignore malformed exported-function markers"
