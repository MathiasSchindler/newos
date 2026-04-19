#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"

phase1_setup() {
    test_name=$1
    WORK_DIR="$ROOT_DIR/tests/tmp/phase1/system/$test_name"
    rm -rf "$WORK_DIR"
    mkdir -p "$WORK_DIR"
    export ROOT_DIR WORK_DIR
    note "phase1 system: $test_name"
}

assert_exit_code() {
    actual=$1
    expected=$2
    description=$3
    [ "$actual" -eq "$expected" ] || fail "$description: expected exit [$expected], got [$actual]"
}

assert_nonempty_text() {
    text=$1
    description=$2
    [ -n "$text" ] || fail "$description"
}
