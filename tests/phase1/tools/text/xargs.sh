#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir xargs)

note "phase1 text: xargs"

printf 'one two\n' | "$ROOT_DIR/build/xargs" "$ROOT_DIR/build/echo" prefix > "$WORK_DIR/basic.out"
basic_out=$(tr -d '\r' < "$WORK_DIR/basic.out" | head -n 1)
assert_text_equals "$basic_out" 'prefix one two' "xargs basic argument composition failed"

printf 'a b c d\n' | "$ROOT_DIR/build/xargs" -n 2 "$ROOT_DIR/build/echo" batch > "$WORK_DIR/batch.out"
cat > "$WORK_DIR/batch.expected" <<'EOF'
batch a b
batch c d
EOF
assert_files_equal "$WORK_DIR/batch.expected" "$WORK_DIR/batch.out" "xargs -n batching failed"

printf 'left side\nnext item\n' | "$ROOT_DIR/build/xargs" -I '{}' "$ROOT_DIR/build/echo" 'pre:{}:post' > "$WORK_DIR/replace.out"
cat > "$WORK_DIR/replace.expected" <<'EOF'
pre:left side:post
pre:next item:post
EOF
assert_files_equal "$WORK_DIR/replace.expected" "$WORK_DIR/replace.out" "xargs -I replacement failed"

printf 'red\0blue sky\0' | "$ROOT_DIR/build/xargs" -0 -n 1 "$ROOT_DIR/build/echo" item > "$WORK_DIR/zero.out"
cat > "$WORK_DIR/zero.expected" <<'EOF'
item red
item blue sky
EOF
assert_files_equal "$WORK_DIR/zero.expected" "$WORK_DIR/zero.out" "xargs -0 parsing failed"

printf '' | "$ROOT_DIR/build/xargs" -r "$ROOT_DIR/build/echo" should-not-run > "$WORK_DIR/norun.out"
norun_out=$(tr -d '\r\n' < "$WORK_DIR/norun.out")
assert_text_equals "$norun_out" '' "xargs -r unexpectedly ran the command for empty input"

printf 'left,right,center' | "$ROOT_DIR/build/xargs" -d , -n 2 "$ROOT_DIR/build/echo" part > "$WORK_DIR/delim.out"
cat > "$WORK_DIR/delim.expected" <<'EOF'
part left right
part center
EOF
assert_files_equal "$WORK_DIR/delim.expected" "$WORK_DIR/delim.out" "xargs -d custom delimiter batching failed"

printf 'trace-me\n' | "$ROOT_DIR/build/xargs" -t -n 1 "$ROOT_DIR/build/echo" show > "$WORK_DIR/trace.out" 2> "$WORK_DIR/trace.err"
assert_file_contains "$WORK_DIR/trace.err" 'show trace-me' "xargs -t did not trace the command invocation"
assert_file_contains "$WORK_DIR/trace.out" '^show trace-me$' "xargs -t changed the executed command output"

if awk 'BEGIN { for (i = 0; i < 300; ++i) printf "x"; printf "\n"; }' | \
    "$ROOT_DIR/build/xargs" "$ROOT_DIR/build/echo" > "$WORK_DIR/long.out" 2> "$WORK_DIR/long.err"
then
    fail "xargs accepted an overlong input argument"
fi
assert_file_contains "$WORK_DIR/long.err" 'xargs: input argument too long' "xargs did not report overlong input safely"

if printf 'abcdef\n' | "$ROOT_DIR/build/xargs" -s 3 "$ROOT_DIR/build/echo" > "$WORK_DIR/maxchars.out" 2> "$WORK_DIR/maxchars.err"
then
    fail "xargs ignored the -s max-chars limit for a single oversized item"
fi
assert_file_contains "$WORK_DIR/maxchars.err" 'xargs: input item exceeds -s limit' "xargs did not reject an item that exceeds -s"
