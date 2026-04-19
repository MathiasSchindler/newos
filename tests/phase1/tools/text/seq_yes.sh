#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir seq_yes)

note "phase1 text: seq/yes"

"$ROOT_DIR/build/seq" 2 2 6 > "$WORK_DIR/seq.out"
printf '2\n4\n6\n' > "$WORK_DIR/seq.expected"
assert_files_equal "$WORK_DIR/seq.expected" "$WORK_DIR/seq.out" "seq did not generate the expected arithmetic progression"

"$ROOT_DIR/build/yes" ok | "$ROOT_DIR/build/head" -n 3 > "$WORK_DIR/yes.out"
printf 'ok\nok\nok\n' > "$WORK_DIR/yes.expected"
assert_files_equal "$WORK_DIR/yes.expected" "$WORK_DIR/yes.out" "yes did not repeat the requested token"
