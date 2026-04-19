#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/ls"
note "phase1 filesystem ls"

mkdir -p "$WORK_DIR/sortdir" "$WORK_DIR/tree/subdir"
printf '1\n' > "$WORK_DIR/sortdir/small"
printf '123456\n' > "$WORK_DIR/sortdir/large"
printf 'nested\n' > "$WORK_DIR/tree/subdir/item.txt"
printf 'root\n' > "$WORK_DIR/tree/root.txt"
ln -sf root.txt "$WORK_DIR/tree/root.link"

"$ROOT_DIR/build/ls" -S "$WORK_DIR/sortdir" > "$WORK_DIR/size.out"
largest_entry=$(head -n 1 "$WORK_DIR/size.out" | tr -d '\r\n')
assert_text_equals "$largest_entry" 'large' "ls -S did not put the larger file first"

"$ROOT_DIR/build/ls" -R "$WORK_DIR/tree" > "$WORK_DIR/recursive.out"
assert_file_contains "$WORK_DIR/recursive.out" 'subdir:' "ls -R did not list the nested directory"

"$ROOT_DIR/build/ls" -1F "$WORK_DIR/tree" > "$WORK_DIR/classify.out"
assert_file_contains "$WORK_DIR/classify.out" '^subdir/$' "ls -F did not mark directories"
assert_file_contains "$WORK_DIR/classify.out" '^root\.link@$' "ls -F did not mark symlinks"

mkdir -p "$WORK_DIR/reverse"
printf 'a\n' > "$WORK_DIR/reverse/a"
printf 'b\n' > "$WORK_DIR/reverse/b"
printf 'c\n' > "$WORK_DIR/reverse/c"
mkdir -p "$WORK_DIR/reverse/dironly"
"$ROOT_DIR/build/ls" -r "$WORK_DIR/reverse" > "$WORK_DIR/reverse.out"
reverse_first=$(head -n 1 "$WORK_DIR/reverse.out" | tr -d '\r\n')
assert_text_equals "$reverse_first" 'dironly' "ls -r did not reverse the output order"

"$ROOT_DIR/build/ls" -isF "$WORK_DIR/tree" > "$WORK_DIR/inodes.out"
assert_file_contains "$WORK_DIR/inodes.out" '^[[:space:]]*[0-9][0-9]*[[:space:]][[:space:]]*[0-9][0-9]*[[:space:]][[:space:]]*root\.txt$' "ls -i/-s did not prefix inode and block counts"
