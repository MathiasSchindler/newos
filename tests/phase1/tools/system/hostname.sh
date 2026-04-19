#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup hostname

hostname_out=$("$ROOT_DIR/build/hostname" | tr -d '\r\n')
assert_nonempty_text "$hostname_out" "hostname output was empty"

uname_nodename_out=$("$ROOT_DIR/build/uname" -n | tr -d '\r\n')
assert_text_equals "$hostname_out" "$uname_nodename_out" "hostname and uname -n disagreed"
