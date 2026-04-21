#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/realpath"
note "phase1 filesystem realpath"

mkdir -p "$WORK_DIR/sub" "$WORK_DIR/policy/base/real/sub"
printf 'data\n' > "$WORK_DIR/dd.in"
ln -sf ../dd.in "$WORK_DIR/sub/link-dd"
ln -sf real/sub "$WORK_DIR/policy/base/linksub"

actual_realpath=$("$ROOT_DIR/build/realpath" "$WORK_DIR/sub/../dd.in" | tr -d '\r\n')
assert_text_equals "$actual_realpath" "$WORK_DIR/dd.in" "realpath normalization failed"

realpath_link=$("$ROOT_DIR/build/realpath" "$WORK_DIR/sub/link-dd" | tr -d '\r\n')
assert_text_equals "$realpath_link" "$WORK_DIR/dd.in" "realpath did not resolve the symlink target"

realpath_missing=$("$ROOT_DIR/build/realpath" -m "$WORK_DIR/sub/missing/../ghost.txt" | tr -d '\r\n')
assert_text_equals "$realpath_missing" "$WORK_DIR/sub/ghost.txt" "realpath -m did not normalize a missing path"

realpath_physical=$("$ROOT_DIR/build/realpath" -P "$WORK_DIR/policy/base/linksub/.." | tr -d '\r\n')
assert_text_equals "$realpath_physical" "$WORK_DIR/policy/base/real" "realpath -P should resolve symlinks before processing .."

realpath_logical=$("$ROOT_DIR/build/realpath" -L "$WORK_DIR/policy/base/linksub/.." | tr -d '\r\n')
assert_text_equals "$realpath_logical" "$WORK_DIR/policy/base" "realpath -L should honor logical traversal through .."

long_component='aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
realpath_long_status=0
"$ROOT_DIR/build/realpath" -m "$WORK_DIR/$long_component/file.txt" >/dev/null 2>&1 || realpath_long_status=$?
[ "$realpath_long_status" -ne 0 ] || fail "realpath should reject an overlong path component instead of truncating it"
