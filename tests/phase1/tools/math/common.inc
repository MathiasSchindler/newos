#!/bin/sh

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_math_setup() {
    name="$1"
    phase1_prepare_workdir "tools/math/$name"
    note "phase1 math: $name"
}
