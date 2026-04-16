#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"

WORK_DIR="$ROOT_DIR/tests/tmp/core_tools"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/src/sub" "$WORK_DIR/grepdir/nested" "$WORK_DIR/lsdir/sub" "$WORK_DIR/finddir/sub"

note "core tools"

printf 'alpha\n' > "$WORK_DIR/src/root.txt"
printf 'beta\n' > "$WORK_DIR/src/sub/file.txt"
assert_command_succeeds "$ROOT_DIR/build/cp" -r "$WORK_DIR/src" "$WORK_DIR/dest"
assert_file_contains "$WORK_DIR/dest/sub/file.txt" beta "cp recursive copy failed"

printf 'Hello\nworld\nHELLO\n' > "$WORK_DIR/grepdir/a.txt"
printf 'hello nested\nskip me\n' > "$WORK_DIR/grepdir/nested/b.txt"
"$ROOT_DIR/build/grep" -ir hello "$WORK_DIR/grepdir" > "$WORK_DIR/grep.out"
assert_file_contains "$WORK_DIR/grep.out" 'a.txt:Hello' "grep recursive ignore-case failed"
assert_file_contains "$WORK_DIR/grep.out" 'b.txt:hello nested' "grep recursive nested match missing"
"$ROOT_DIR/build/grep" -v skip "$WORK_DIR/grepdir/nested/b.txt" > "$WORK_DIR/grep_v.out"
assert_file_contains "$WORK_DIR/grep_v.out" 'hello nested' "grep invert-match failed"

printf '123456\n' > "$WORK_DIR/lsdir/large"
printf '1\n' > "$WORK_DIR/lsdir/small"
touch -t 202401010101 "$WORK_DIR/lsdir/small"
touch -t 202501010101 "$WORK_DIR/lsdir/large"
"$ROOT_DIR/build/ls" -R "$WORK_DIR/lsdir" > "$WORK_DIR/ls_r.out"
assert_file_contains "$WORK_DIR/ls_r.out" 'lsdir/sub:' "ls recursive listing failed"
"$ROOT_DIR/build/ls" -S "$WORK_DIR/lsdir" > "$WORK_DIR/ls_s.out"
assert_file_contains "$WORK_DIR/ls_s.out" '^large$' "ls size ordering missing"

printf '10\n2\n2\n1\n' > "$WORK_DIR/sort.txt"
"$ROOT_DIR/build/sort" -nru "$WORK_DIR/sort.txt" > "$WORK_DIR/sort.out"
printf '10\n2\n1\n' > "$WORK_DIR/sort.expected"
assert_files_equal "$WORK_DIR/sort.out" "$WORK_DIR/sort.expected" "sort numeric reverse unique failed"

printf 'abc\n' > "$WORK_DIR/finddir/small.txt"
printf 'abcdef\n' > "$WORK_DIR/finddir/big.txt"
"$ROOT_DIR/build/find" "$WORK_DIR/finddir" -type f -size +3c > "$WORK_DIR/find_size.out"
assert_file_contains "$WORK_DIR/find_size.out" 'big.txt' "find size filter failed"

printf 'foo\nfoo\nfoo\n' > "$WORK_DIR/sed.txt"
printf '2s/foo/BAR/\n' > "$WORK_DIR/script.sed"
"$ROOT_DIR/build/sed" -f "$WORK_DIR/script.sed" "$WORK_DIR/sed.txt" > "$WORK_DIR/sed.out"
printf 'foo\nBAR\nfoo\n' > "$WORK_DIR/sed.expected"
assert_files_equal "$WORK_DIR/sed.out" "$WORK_DIR/sed.expected" "sed addressed script failed"
