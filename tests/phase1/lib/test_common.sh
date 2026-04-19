#!/bin/sh

[ -n "${ROOT_DIR:-}" ] || ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"

phase1_prepare_workdir() {
    test_name="$1"
    WORK_DIR="$ROOT_DIR/tests/tmp/phase1/$test_name"
    export WORK_DIR
    rm -rf "$WORK_DIR"
    mkdir -p "$WORK_DIR"
}
