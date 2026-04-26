#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir expand_unexpand)

note "phase1 text: expand/unexpand"

printf 'a\tb\n' > "$WORK_DIR/tabs.txt"
"$ROOT_DIR/build/expand" -t 4 "$WORK_DIR/tabs.txt" > "$WORK_DIR/expand.out"
printf 'a   b\n' > "$WORK_DIR/expand.expected"
assert_files_equal "$WORK_DIR/expand.expected" "$WORK_DIR/expand.out" "expand -t 4 did not expand a tab to the expected stop"

printf '\tlead\tkeep\n' > "$WORK_DIR/initial_tabs.txt"
"$ROOT_DIR/build/expand" -i -t 4 "$WORK_DIR/initial_tabs.txt" > "$WORK_DIR/expand_initial.out"
printf '    lead\tkeep\n' > "$WORK_DIR/expand_initial.expected"
assert_files_equal "$WORK_DIR/expand_initial.expected" "$WORK_DIR/expand_initial.out" "expand -i did not restrict expansion to leading tabs"

printf '        indent\n' > "$WORK_DIR/spaces.txt"
"$ROOT_DIR/build/unexpand" "$WORK_DIR/spaces.txt" > "$WORK_DIR/unexpand.out"
printf '\tindent\n' > "$WORK_DIR/unexpand.expected"
assert_files_equal "$WORK_DIR/unexpand.expected" "$WORK_DIR/unexpand.out" "unexpand did not convert leading spaces to a tab"

printf 'a   b\n' > "$WORK_DIR/all_spaces.txt"
"$ROOT_DIR/build/unexpand" -a -t 4 "$WORK_DIR/all_spaces.txt" > "$WORK_DIR/unexpand_all.out"
printf 'a\tb\n' > "$WORK_DIR/unexpand_all.expected"
assert_files_equal "$WORK_DIR/unexpand_all.expected" "$WORK_DIR/unexpand_all.out" "unexpand -a -t 4 did not convert an interior aligned space run"