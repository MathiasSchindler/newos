#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup uname

uname_out=$("$ROOT_DIR/build/uname" -snrm | tr -d '\r\n')
assert_nonempty_text "$uname_out" "uname produced no output"

case "$uname_out" in
    *" "*) ;;
    *) fail "uname -snrm should include multiple fields" ;;
esac

uname_version_out=$("$ROOT_DIR/build/uname" --kernel-version | tr -d '\r\n')
assert_nonempty_text "$uname_version_out" "uname --kernel-version produced no output"
