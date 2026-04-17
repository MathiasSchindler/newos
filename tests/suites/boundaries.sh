#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"

WORK_DIR="$ROOT_DIR/tests/tmp/boundaries"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

note "boundary conditions"

awk 'BEGIN { for (i = 0; i < 7000; ++i) printf "A"; printf "MARKER\n"; }' > "$WORK_DIR/long.txt"
"$ROOT_DIR/build/grep" MARKER "$WORK_DIR/long.txt" > "$WORK_DIR/long_grep.out"
assert_file_contains "$WORK_DIR/long_grep.out" 'MARKER' "grep long-line handling failed"

DEEP_SRC="$WORK_DIR/deep_src"
DEEP_REL=""
level=1
while [ "$level" -le 12 ]; do
    DEEP_REL="$DEEP_REL/level_$level"
    mkdir -p "$DEEP_SRC$DEEP_REL"
    level=$((level + 1))
done
printf 'deep-data\n' > "$DEEP_SRC$DEEP_REL/final.txt"
"$ROOT_DIR/build/find" "$DEEP_SRC" -name final.txt > "$WORK_DIR/deep_find.out"
assert_file_contains "$WORK_DIR/deep_find.out" 'final.txt' "find deep-directory traversal failed"
assert_command_succeeds "$ROOT_DIR/build/cp" -r "$DEEP_SRC" "$WORK_DIR/deep_copy"
assert_file_contains "$WORK_DIR/deep_copy$DEEP_REL/final.txt" 'deep-data' "cp deep recursive copy failed"

ODD_FILE="$WORK_DIR/odd name [v1] !.txt"
printf 'odd-data\n' > "$ODD_FILE"
assert_command_succeeds "$ROOT_DIR/build/cp" "$ODD_FILE" "$WORK_DIR/odd-copy.txt"
assert_file_contains "$WORK_DIR/odd-copy.txt" 'odd-data' "cp odd filename failed"
"$ROOT_DIR/build/cat" "$ODD_FILE" > "$WORK_DIR/odd_cat.out"
assert_file_contains "$WORK_DIR/odd_cat.out" '^odd-data$' "cat odd filename failed"

: > "$WORK_DIR/large_numbers.txt"
value=1500
while [ "$value" -ge 1 ]; do
    printf '%s\n' "$value" >> "$WORK_DIR/large_numbers.txt"
    value=$((value - 1))
done
"$ROOT_DIR/build/sort" -n "$WORK_DIR/large_numbers.txt" > "$WORK_DIR/large_sorted.out"
first_sorted=$(head -n 1 "$WORK_DIR/large_sorted.out" | tr -d '\r\n')
last_sorted=$(tail -n 1 "$WORK_DIR/large_sorted.out" | tr -d '\r\n')
assert_text_equals "$first_sorted" '1' "sort large-input start ordering failed"
assert_text_equals "$last_sorted" '1500' "sort large-input end ordering failed"
