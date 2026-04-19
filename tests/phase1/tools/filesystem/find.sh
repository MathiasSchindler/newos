#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/find"
note "phase1 filesystem find"

mkdir -p "$WORK_DIR/tree/keep" "$WORK_DIR/tree/skip/deeper"
printf 'keep\n' > "$WORK_DIR/tree/keep/kept.txt"
printf 'skip\n' > "$WORK_DIR/tree/skip/deeper/skipped.txt"
ln -sf keep/kept.txt "$WORK_DIR/tree/kept.link"

"$ROOT_DIR/build/find" "$WORK_DIR/tree" -maxdepth 1 > "$WORK_DIR/maxdepth.out"
assert_file_contains "$WORK_DIR/maxdepth.out" 'tree$' "find -maxdepth did not include the starting directory"
if grep 'kept.txt\|skipped.txt' "$WORK_DIR/maxdepth.out" >/dev/null 2>&1; then
    fail "find -maxdepth descended too deeply"
fi

"$ROOT_DIR/build/find" "$WORK_DIR/tree" -mindepth 1 -type f -print0 | tr '\0' '\n' > "$WORK_DIR/print0.out"
assert_file_contains "$WORK_DIR/print0.out" 'kept.txt' "find -mindepth/-print0 missed the shallow file"
assert_file_contains "$WORK_DIR/print0.out" 'skipped.txt' "find -mindepth/-print0 missed the nested file"

"$ROOT_DIR/build/find" "$WORK_DIR/tree" -type l > "$WORK_DIR/links.out"
assert_file_contains "$WORK_DIR/links.out" 'kept.link' "find -type l did not report symlinks"

"$ROOT_DIR/build/find" "$WORK_DIR/tree" -name kept.txt -exec "$ROOT_DIR/build/echo" found {} ';' > "$WORK_DIR/exec.out"
assert_file_contains "$WORK_DIR/exec.out" 'found .*kept.txt' "find -exec did not run the requested command"

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
assert_file_contains "$WORK_DIR/deep_find.out" 'final.txt' "find did not traverse a deeply nested directory"
