#!/bin/sh
set -eu

PHASE1_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Drop tool or group scripts into tests/phase1/<group>/*.sh.
. "$PHASE1_DIR/lib/runner.sh"

run_phase1_tests "$@"
