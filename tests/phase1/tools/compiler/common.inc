#!/bin/sh

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

RUN_TARGET=""
case "$(uname -s):$(uname -m)" in
    Darwin:arm64|Darwin:aarch64)
        RUN_TARGET="macos-aarch64"
        ;;
    Linux:x86_64)
        RUN_TARGET="linux-x86_64"
        ;;
esac

phase1_compiler_setup() {
    name="$1"
    phase1_prepare_workdir "tools/compiler/$name"
    note "phase1 compiler: $name"
}

compile_and_check_native() {
    source_path=$1
    output_path=$2
    expected_status=$3
    message=$4
    actual_status=0

    if [ -z "$RUN_TARGET" ]; then
        return 0
    fi

    "$ROOT_DIR/build/ncc" --target "$RUN_TARGET" "$source_path" -o "$output_path"
    if "$output_path"; then
        actual_status=0
    else
        actual_status=$?
    fi
    assert_text_equals "$actual_status" "$expected_status" "$message"
}
