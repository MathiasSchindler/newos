#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/find"
note "phase1 filesystem find"

mkdir -p "$WORK_DIR/tree/keep" "$WORK_DIR/tree/skip/deeper"
printf 'keep\n' > "$WORK_DIR/tree/keep/kept.txt"
printf 'skip\n' > "$WORK_DIR/tree/skip/deeper/skipped.txt"
: > "$WORK_DIR/tree/keep/empty.LOG"
mkdir -p "$WORK_DIR/tree/emptydir"
ln -sf keep/kept.txt "$WORK_DIR/tree/kept.link"

"${TEST_BIN_DIR}/find" "$WORK_DIR/tree" -maxdepth 1 > "$WORK_DIR/maxdepth.out"
assert_file_contains "$WORK_DIR/maxdepth.out" 'tree$' "find -maxdepth did not include the starting directory"
if grep 'kept.txt\|skipped.txt' "$WORK_DIR/maxdepth.out" >/dev/null 2>&1; then
    fail "find -maxdepth descended too deeply"
fi

"${TEST_BIN_DIR}/find" "$WORK_DIR/tree" -mindepth 1 -type f -print0 | tr '\0' '\n' > "$WORK_DIR/print0.out"
assert_file_contains "$WORK_DIR/print0.out" 'kept.txt' "find -mindepth/-print0 missed the shallow file"
assert_file_contains "$WORK_DIR/print0.out" 'skipped.txt' "find -mindepth/-print0 missed the nested file"

"${TEST_BIN_DIR}/find" "$WORK_DIR/tree" -type l > "$WORK_DIR/links.out"
assert_file_contains "$WORK_DIR/links.out" 'kept.link' "find -type l did not report symlinks"

"${TEST_BIN_DIR}/find" "$WORK_DIR/tree" -name kept.txt -exec "${TEST_BIN_DIR}/echo" found {} ';' > "$WORK_DIR/exec.out"
assert_file_contains "$WORK_DIR/exec.out" 'found .*kept.txt' "find -exec did not run the requested command"

"${TEST_BIN_DIR}/find" "$WORK_DIR/tree" -empty > "$WORK_DIR/empty.out"
assert_file_contains "$WORK_DIR/empty.out" 'empty.LOG' "find -empty did not report the empty file"
assert_file_contains "$WORK_DIR/empty.out" 'emptydir' "find -empty did not report the empty directory"

chmod 600 "$WORK_DIR/tree/keep/kept.txt"
"${TEST_BIN_DIR}/find" "$WORK_DIR/tree" -type f -perm 600 > "$WORK_DIR/perm_exact.out"
assert_file_contains "$WORK_DIR/perm_exact.out" 'kept.txt' "find -perm exact mode did not match the chmodded file"

"${TEST_BIN_DIR}/find" "$WORK_DIR/tree" -type f -perm -600 > "$WORK_DIR/perm_all.out"
assert_file_contains "$WORK_DIR/perm_all.out" 'kept.txt' "find -perm -MODE did not match required bits"

"${TEST_BIN_DIR}/find" "$WORK_DIR/tree" -user "$(id -u)" -name kept.txt > "$WORK_DIR/user.out"
assert_file_contains "$WORK_DIR/user.out" 'kept.txt' "find -user did not match the numeric owner"

"${TEST_BIN_DIR}/find" "$WORK_DIR/tree" -group "$(id -g)" -name kept.txt > "$WORK_DIR/group.out"
assert_file_contains "$WORK_DIR/group.out" 'kept.txt' "find -group did not match the numeric group"

printf 'old\n' > "$WORK_DIR/tree/old.txt"
printf 'new\n' > "$WORK_DIR/tree/new.txt"
touch -t 202001010000 "$WORK_DIR/tree/old.txt"
touch -t 202101010000 "$WORK_DIR/tree/new.txt"
"${TEST_BIN_DIR}/find" "$WORK_DIR/tree" -newer "$WORK_DIR/tree/old.txt" -name new.txt > "$WORK_DIR/newer.out"
assert_file_contains "$WORK_DIR/newer.out" 'new.txt' "find -newer did not match a newer file"

"${TEST_BIN_DIR}/find" "$WORK_DIR/tree" -type d -name skip -prune -o -iname 'KEPT.TXT' -print > "$WORK_DIR/logic.out"
assert_file_contains "$WORK_DIR/logic.out" 'kept.txt' "find logical operators or -iname matching failed"
if grep 'skipped.txt' "$WORK_DIR/logic.out" >/dev/null 2>&1; then
    fail "find -prune/-o still descended into the skipped directory"
fi

"${TEST_BIN_DIR}/find" "$WORK_DIR/tree" -type f ! -iname '*.txt' > "$WORK_DIR/not.out"
assert_file_contains "$WORK_DIR/not.out" 'empty.LOG' "find ! -iname did not keep the non-text file"
if grep 'kept.txt\|skipped.txt' "$WORK_DIR/not.out" >/dev/null 2>&1; then
    fail "find ! -iname did not exclude the text files"
fi

DEEP_SRC="$WORK_DIR/deep_src"
DEEP_REL=""
level=1
while [ "$level" -le 12 ]; do
    DEEP_REL="$DEEP_REL/level_$level"
    mkdir -p "$DEEP_SRC$DEEP_REL"
    level=$((level + 1))
done
printf 'deep-data\n' > "$DEEP_SRC$DEEP_REL/final.txt"

"${TEST_BIN_DIR}/find" "$DEEP_SRC" -name final.txt > "$WORK_DIR/deep_find.out"
assert_file_contains "$WORK_DIR/deep_find.out" 'final.txt' "find did not traverse a deeply nested directory"
