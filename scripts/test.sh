#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NEWOS_TEST_PORT=${NEWOS_TEST_PORT:-$((39000 + ($$ % 1000)))}

cd "$ROOT_DIR"
"$ROOT_DIR/scripts/run-userland.sh" "$@" -- "export NEWOS_TEST_PORT=$NEWOS_TEST_PORT; sh tests/suites/userland.sh"