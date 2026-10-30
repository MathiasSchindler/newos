#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "platform/fontrender"
note "phase1 platform: font renderer"

case "$TEST_BIN_DIR" in
    */host-*) build_variable=BUILD_DIR ;;
    *)
        case "$(uname -s):$(uname -m)" in
            Darwin:arm64|Darwin:aarch64) build_variable=MACOS_BUILD_DIR ;;
            Linux:*) build_variable=TARGET_BUILD_DIR ;;
            *) build_variable=BUILD_DIR ;;
        esac
        ;;
esac
assert_command_succeeds make -C "$ROOT_DIR" --no-print-directory \
    "$build_variable=$TEST_BIN_DIR" "$TEST_BIN_DIR/fontrendertest"
if ! "$TEST_BIN_DIR/fontrendertest" "$WORK_DIR/font.ttf" > "$WORK_DIR/result.out" 2> "$WORK_DIR/result.err"; then
    cat "$WORK_DIR/result.out" "$WORK_DIR/result.err"
    fail "font renderer regression failed"
fi
assert_file_contains "$WORK_DIR/result.out" '^fontrender tests passed$' "font renderer regression failed"
assert_file_contains "$WORK_DIR/result.err" 'malformed font' "font renderer did not report malformed input"
assert_file_contains "$WORK_DIR/result.err" 'missing U+10FFFF' "font renderer did not report a missing fallback"
