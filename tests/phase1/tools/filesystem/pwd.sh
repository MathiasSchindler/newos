#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/pwd"
note "phase1 filesystem pwd"

mkdir -p "$WORK_DIR/real/sub"
ln -sf real "$WORK_DIR/link"

pwd_logical=$(
    cd "$WORK_DIR/link/sub"
    PWD="$WORK_DIR/link/sub" "$ROOT_DIR/build/pwd" -L | tr -d '\r\n'
)
assert_text_equals "$pwd_logical" "$WORK_DIR/link/sub" "pwd -L did not honor a valid logical path"

pwd_physical=$(
    cd "$WORK_DIR/link/sub"
    PWD="$WORK_DIR/link/sub" "$ROOT_DIR/build/pwd" -P | tr -d '\r\n'
)
assert_text_equals "$pwd_physical" "$WORK_DIR/real/sub" "pwd -P did not print the physical working directory"
