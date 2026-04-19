#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup sleep

assert_command_succeeds "$ROOT_DIR/build/sleep" 0 0.01s 0.02

if "$ROOT_DIR/build/sleep" nope >/dev/null 2>&1; then
    fail "sleep should reject an invalid duration"
fi
