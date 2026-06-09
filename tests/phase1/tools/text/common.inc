#!/bin/sh

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_text_workdir() {
    name="$1"
    work_dir="$ROOT_DIR/tests/tmp/phase1/text/$name"
    rm -rf "$work_dir"
    mkdir -p "$work_dir"
    export ROOT_DIR TEST_BIN_DIR
    printf '%s\n' "$work_dir"
}
