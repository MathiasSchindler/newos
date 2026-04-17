#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

mkdir -p "$ROOT_DIR/tests/tmp"

"$ROOT_DIR/tests/suites/core_tools.sh"
"$ROOT_DIR/tests/suites/extended_tools.sh"
"$ROOT_DIR/tests/suites/compiler.sh"
"$ROOT_DIR/tests/suites/shell.sh"
sh "$ROOT_DIR/tests/suites/boundaries.sh"

echo "ALL_TEST_SUITES_OK"
