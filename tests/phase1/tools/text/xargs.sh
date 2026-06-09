#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir xargs)

note "phase1 text: xargs"

printf 'one two\n' | "${TEST_BIN_DIR}/xargs" "${TEST_BIN_DIR}/echo" prefix > "$WORK_DIR/basic.out"
basic_out=$(tr -d '\r' < "$WORK_DIR/basic.out" | head -n 1)
assert_text_equals "$basic_out" 'prefix one two' "xargs basic argument composition failed"

printf 'a b c d\n' | "${TEST_BIN_DIR}/xargs" -n 2 "${TEST_BIN_DIR}/echo" batch > "$WORK_DIR/batch.out"
cat > "$WORK_DIR/batch.expected" <<'EOF'
batch a b
batch c d
EOF
assert_files_equal "$WORK_DIR/batch.expected" "$WORK_DIR/batch.out" "xargs -n batching failed"

printf 'left side\nnext item\n' | "${TEST_BIN_DIR}/xargs" -I '{}' "${TEST_BIN_DIR}/echo" 'pre:{}:post' > "$WORK_DIR/replace.out"
cat > "$WORK_DIR/replace.expected" <<'EOF'
pre:left side:post
pre:next item:post
EOF
assert_files_equal "$WORK_DIR/replace.expected" "$WORK_DIR/replace.out" "xargs -I replacement failed"

printf 'red\0blue sky\0' | "${TEST_BIN_DIR}/xargs" -0 -n 1 "${TEST_BIN_DIR}/echo" item > "$WORK_DIR/zero.out"
cat > "$WORK_DIR/zero.expected" <<'EOF'
item red
item blue sky
EOF
assert_files_equal "$WORK_DIR/zero.expected" "$WORK_DIR/zero.out" "xargs -0 parsing failed"

printf '' | "${TEST_BIN_DIR}/xargs" -r "${TEST_BIN_DIR}/echo" should-not-run > "$WORK_DIR/norun.out"
norun_out=$(tr -d '\r\n' < "$WORK_DIR/norun.out")
assert_text_equals "$norun_out" '' "xargs -r unexpectedly ran the command for empty input"

printf 'left,right,center' | "${TEST_BIN_DIR}/xargs" -d , -n 2 "${TEST_BIN_DIR}/echo" part > "$WORK_DIR/delim.out"
cat > "$WORK_DIR/delim.expected" <<'EOF'
part left right
part center
EOF
assert_files_equal "$WORK_DIR/delim.expected" "$WORK_DIR/delim.out" "xargs -d custom delimiter batching failed"

printf 'trace-me\n' | "${TEST_BIN_DIR}/xargs" -t -n 1 "${TEST_BIN_DIR}/echo" show > "$WORK_DIR/trace.out" 2> "$WORK_DIR/trace.err"
assert_file_contains "$WORK_DIR/trace.err" 'show trace-me' "xargs -t did not trace the command invocation"
assert_file_contains "$WORK_DIR/trace.out" '^show trace-me$' "xargs -t changed the executed command output"

if awk 'BEGIN { for (i = 0; i < 300; ++i) printf "x"; printf "\n"; }' | \
    "${TEST_BIN_DIR}/xargs" "${TEST_BIN_DIR}/echo" > "$WORK_DIR/long.out" 2> "$WORK_DIR/long.err"
then
    fail "xargs accepted an overlong input argument"
fi
assert_file_contains "$WORK_DIR/long.err" 'xargs: input argument too long' "xargs did not report overlong input safely"

if printf 'abcdef\n' | "${TEST_BIN_DIR}/xargs" -s 3 "${TEST_BIN_DIR}/echo" > "$WORK_DIR/maxchars.out" 2> "$WORK_DIR/maxchars.err"
then
    fail "xargs ignored the -s max-chars limit for a single oversized item"
fi
assert_file_contains "$WORK_DIR/maxchars.err" 'xargs: input item exceeds -s limit' "xargs did not reject an item that exceeds -s"
