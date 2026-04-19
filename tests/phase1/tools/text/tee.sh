#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir tee)

note "phase1 text: tee"

printf 'hello tee\n' | "$ROOT_DIR/build/tee" "$WORK_DIR/out.txt" > "$WORK_DIR/stdout.txt"
assert_files_equal "$WORK_DIR/out.txt" "$WORK_DIR/stdout.txt" "tee did not mirror input to both stdout and the file"

printf 'first\n' > "$WORK_DIR/append.txt"
printf 'second\n' | "$ROOT_DIR/build/tee" -a "$WORK_DIR/append.txt" > /dev/null
cat > "$WORK_DIR/append.expected" <<'EOF'
first
second
EOF
assert_files_equal "$WORK_DIR/append.expected" "$WORK_DIR/append.txt" "tee -a did not append to the existing file"

tee_status=0
printf 'partial write\n' | "$ROOT_DIR/build/tee" -i "$WORK_DIR/partial.txt" "$WORK_DIR/missing/fail.txt" > "$WORK_DIR/partial.out" || tee_status=$?
[ "$tee_status" -ne 0 ] || fail "tee should return a failure status when one output cannot be opened"
assert_file_contains "$WORK_DIR/partial.txt" '^partial write$' "tee did not preserve the healthy destination after a partial failure"
assert_file_contains "$WORK_DIR/partial.out" '^partial write$' "tee did not preserve stdout output after a partial failure"
