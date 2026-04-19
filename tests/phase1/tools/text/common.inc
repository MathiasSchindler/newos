#!/bin/sh

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"

phase1_text_workdir() {
    name="$1"
    work_dir="$ROOT_DIR/tests/tmp/phase1/text/$name"
    rm -rf "$work_dir"
    mkdir -p "$work_dir"
    printf '%s\n' "$work_dir"
}
