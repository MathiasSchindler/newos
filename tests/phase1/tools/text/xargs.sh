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
