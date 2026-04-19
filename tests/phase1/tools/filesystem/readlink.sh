#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/readlink"
note "phase1 filesystem readlink"

mkdir -p "$WORK_DIR/sub"
printf 'data\n' > "$WORK_DIR/dd.in"
ln -sf dd.in "$WORK_DIR/link-to-dd"
ln -sf ../dd.in "$WORK_DIR/sub/link-dd"

readlink_out=$("$ROOT_DIR/build/readlink" "$WORK_DIR/link-to-dd" | tr -d '\r\n')
assert_text_equals "$readlink_out" 'dd.in' "readlink returned the wrong symlink target"

readlink_full=$("$ROOT_DIR/build/readlink" -f "$WORK_DIR/sub/link-dd" | tr -d '\r\n')
assert_text_equals "$readlink_full" "$WORK_DIR/dd.in" "readlink -f did not canonicalize the symlink"

readlink_existing=$("$ROOT_DIR/build/readlink" --canonicalize-existing "$WORK_DIR/sub/link-dd" | tr -d '\r\n')
assert_text_equals "$readlink_existing" "$WORK_DIR/dd.in" "readlink --canonicalize-existing regressed"

readlink_missing_status=0
"$ROOT_DIR/build/readlink" -f "$WORK_DIR/sub/missing-parent/ghost.txt" >/dev/null 2>&1 || readlink_missing_status=$?
[ "$readlink_missing_status" -ne 0 ] || fail "readlink -f should reject paths with missing parent directories"

readlink_verbose=$("$ROOT_DIR/build/readlink" -v "$WORK_DIR/link-to-dd" | tr -d '\r\n')
assert_text_equals "$readlink_verbose" "$WORK_DIR/link-to-dd: dd.in" "readlink -v did not prefix the source path"
