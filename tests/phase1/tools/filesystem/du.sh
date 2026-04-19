#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/du"
note "phase1 filesystem du"

mkdir -p "$WORK_DIR/tree/sub"
printf '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef' > "$WORK_DIR/tree/a.txt"
printf 'abcdef' > "$WORK_DIR/tree/sub/b.txt"
ln -sf a.txt "$WORK_DIR/tree/a.link"

"$ROOT_DIR/build/du" "$WORK_DIR/tree" > "$WORK_DIR/basic.out"
assert_file_contains "$WORK_DIR/basic.out" 'tree$' "du did not report the directory total"

du_h_out=$("$ROOT_DIR/build/du" -sh "$WORK_DIR/tree" | tr -d '\r')
printf '%s\n' "$du_h_out" > "$WORK_DIR/human.out"
assert_file_contains "$WORK_DIR/human.out" '^[0-9][0-9.]*[BKMGTP][[:space:]]' "du -h did not produce human-readable sizes"

"$ROOT_DIR/build/du" -a "$WORK_DIR/tree" > "$WORK_DIR/all.out"
assert_file_contains "$WORK_DIR/all.out" 'tree/a.txt$' "du -a did not include file entries"

"$ROOT_DIR/build/du" -d 0 "$WORK_DIR/tree" > "$WORK_DIR/depth.out"
if grep 'tree/a.txt$' "$WORK_DIR/depth.out" >/dev/null 2>&1; then
    fail "du -d 0 should suppress deeper child entries"
fi

"$ROOT_DIR/build/du" -ac "$WORK_DIR/tree" > "$WORK_DIR/total.out"
assert_file_contains "$WORK_DIR/total.out" '[[:space:]]total$' "du -c did not print a grand total"

set -- $("$ROOT_DIR/build/du" -sb "$WORK_DIR/tree/a.link" | tr -d '\r')
du_link_plain=$1
set -- $("$ROOT_DIR/build/du" -sLb "$WORK_DIR/tree/a.link" | tr -d '\r')
du_link_follow=$1
assert_text_equals "$du_link_follow" '64' "du -L -b should report the target file size"
if [ "$du_link_plain" -ge "$du_link_follow" ]; then
    fail "du without -L should measure the symlink itself rather than the target contents"
fi
