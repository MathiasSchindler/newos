#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/compression"
note "phase1 filesystem compression"

printf 'alpha\nbeta\n' > "$WORK_DIR/sample.txt"

assert_command_succeeds "${TEST_BIN_DIR}/gzip" -c "$WORK_DIR/sample.txt" > "$WORK_DIR/sample.txt.gz"
assert_command_succeeds "${TEST_BIN_DIR}/gunzip" -c "$WORK_DIR/sample.txt.gz" > "$WORK_DIR/gzip.out"
assert_files_equal "$WORK_DIR/sample.txt" "$WORK_DIR/gzip.out" "gzip/gunzip -c did not preserve file contents"

cp "$WORK_DIR/sample.txt" "$WORK_DIR/bzip-input.txt"
assert_command_succeeds "${TEST_BIN_DIR}/bzip2" "$WORK_DIR/bzip-input.txt"
[ -f "$WORK_DIR/bzip-input.txt.bz2" ] || fail "bzip2 did not create a .bz2 file"
assert_command_succeeds "${TEST_BIN_DIR}/bunzip2" "$WORK_DIR/bzip-input.txt.bz2"
assert_files_equal "$WORK_DIR/sample.txt" "$WORK_DIR/bzip-input.txt" "bunzip2 did not restore the original data"

if command -v python3 >/dev/null 2>&1 && python3 - <<'PY' >/dev/null 2>&1
import bz2
PY
then
	python3 - "$WORK_DIR/sample.txt" "$WORK_DIR/real-bzip.txt.bz2" <<'PY'
import bz2, pathlib, sys
pathlib.Path(sys.argv[2]).write_bytes(bz2.compress(pathlib.Path(sys.argv[1]).read_bytes()))
PY
	assert_command_succeeds "${TEST_BIN_DIR}/bunzip2" "$WORK_DIR/real-bzip.txt.bz2"
	assert_files_equal "$WORK_DIR/sample.txt" "$WORK_DIR/real-bzip.txt" "bunzip2 did not decode a standard bzip2 stream"
fi

cp "$WORK_DIR/sample.txt" "$WORK_DIR/xz-input.txt"
assert_command_succeeds "${TEST_BIN_DIR}/xz" "$WORK_DIR/xz-input.txt"
[ -f "$WORK_DIR/xz-input.txt.xz" ] || fail "xz did not create a .xz file"
assert_command_succeeds "${TEST_BIN_DIR}/unxz" "$WORK_DIR/xz-input.txt.xz"
assert_files_equal "$WORK_DIR/sample.txt" "$WORK_DIR/xz-input.txt" "unxz did not restore the original data"
