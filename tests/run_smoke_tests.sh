#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LOG_DIR="$ROOT_DIR/tests/tmp/logs"

mkdir -p "$ROOT_DIR/tests/tmp"
rm -rf "$LOG_DIR"
mkdir -p "$LOG_DIR"

run_suite() {
    suite_name=$1
    script_path=$2
    log_path="$LOG_DIR/$suite_name.log"
    sh "$script_path" >"$log_path" 2>&1 &
    eval "${suite_name}_pid=$!"
}

run_suite core_tools "$ROOT_DIR/tests/suites/core_tools.sh"
run_suite extended_tools "$ROOT_DIR/tests/suites/extended_tools.sh"
run_suite compiler "$ROOT_DIR/tests/suites/compiler.sh"
run_suite shell "$ROOT_DIR/tests/suites/shell.sh"
run_suite boundaries "$ROOT_DIR/tests/suites/boundaries.sh"
run_suite bignum "$ROOT_DIR/tests/suites/bignum.sh"

status=0
for suite_name in core_tools extended_tools compiler shell boundaries bignum; do
    eval "suite_pid=\${${suite_name}_pid}"
    log_path="$LOG_DIR/$suite_name.log"
    if ! wait "$suite_pid"; then
        status=1
    fi
    cat "$log_path"
done

if [ "$status" -ne 0 ]; then
    exit "$status"
fi

echo "ALL_TEST_SUITES_OK"
