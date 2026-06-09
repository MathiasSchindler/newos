#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir expand_unexpand)

note "phase1 text: expand/unexpand"

printf 'a\tb\n' > "$WORK_DIR/tabs.txt"
"${TEST_BIN_DIR}/expand" -t 4 "$WORK_DIR/tabs.txt" > "$WORK_DIR/expand.out"
printf 'a   b\n' > "$WORK_DIR/expand.expected"
assert_files_equal "$WORK_DIR/expand.expected" "$WORK_DIR/expand.out" "expand -t 4 did not expand a tab to the expected stop"

printf '\tlead\tkeep\n' > "$WORK_DIR/initial_tabs.txt"
"${TEST_BIN_DIR}/expand" -i -t 4 "$WORK_DIR/initial_tabs.txt" > "$WORK_DIR/expand_initial.out"
printf '    lead\tkeep\n' > "$WORK_DIR/expand_initial.expected"
assert_files_equal "$WORK_DIR/expand_initial.expected" "$WORK_DIR/expand_initial.out" "expand -i did not restrict expansion to leading tabs"

printf '        indent\n' > "$WORK_DIR/spaces.txt"
"${TEST_BIN_DIR}/unexpand" "$WORK_DIR/spaces.txt" > "$WORK_DIR/unexpand.out"
printf '\tindent\n' > "$WORK_DIR/unexpand.expected"
assert_files_equal "$WORK_DIR/unexpand.expected" "$WORK_DIR/unexpand.out" "unexpand did not convert leading spaces to a tab"

printf 'a   b\n' > "$WORK_DIR/all_spaces.txt"
"${TEST_BIN_DIR}/unexpand" -a -t 4 "$WORK_DIR/all_spaces.txt" > "$WORK_DIR/unexpand_all.out"
printf 'a\tb\n' > "$WORK_DIR/unexpand_all.expected"
assert_files_equal "$WORK_DIR/unexpand_all.expected" "$WORK_DIR/unexpand_all.out" "unexpand -a -t 4 did not convert an interior aligned space run"

printf 'a\tb\0c\td\0' | "${TEST_BIN_DIR}/expand" -z -t 4 | tr '\0' '\n' > "$WORK_DIR/expand_zero.out"
printf 'a   b\nc   d\n' > "$WORK_DIR/expand_zero.expected"
assert_files_equal "$WORK_DIR/expand_zero.expected" "$WORK_DIR/expand_zero.out" "expand -z did not reset columns at NUL records"

printf 'a   b\0c   d\0' | "${TEST_BIN_DIR}/unexpand" -z -a -t 4 | tr '\0' '\n' > "$WORK_DIR/unexpand_zero.out"
printf 'a\tb\nc\td\n' > "$WORK_DIR/unexpand_zero.expected"
assert_files_equal "$WORK_DIR/unexpand_zero.expected" "$WORK_DIR/unexpand_zero.out" "unexpand -z did not reset columns at NUL records"

printf '界\tz\n' > "$WORK_DIR/wide_tabs.txt"
"${TEST_BIN_DIR}/expand" -t 4 "$WORK_DIR/wide_tabs.txt" > "$WORK_DIR/expand_wide.out"
printf '界  z\n' > "$WORK_DIR/expand_wide.expected"
assert_files_equal "$WORK_DIR/expand_wide.expected" "$WORK_DIR/expand_wide.out" "expand did not use wide Unicode display columns before a tab"

printf 'é\tz\n' > "$WORK_DIR/combining_tabs.txt"
"${TEST_BIN_DIR}/expand" -t 4 "$WORK_DIR/combining_tabs.txt" > "$WORK_DIR/expand_combining.out"
printf 'é   z\n' > "$WORK_DIR/expand_combining.expected"
assert_files_equal "$WORK_DIR/expand_combining.expected" "$WORK_DIR/expand_combining.out" "expand counted a combining mark as a tab column"

printf '界  z\n' > "$WORK_DIR/unexpand_wide.txt"
"${TEST_BIN_DIR}/unexpand" -a -t 4 "$WORK_DIR/unexpand_wide.txt" > "$WORK_DIR/unexpand_wide.out"
printf '界\tz\n' > "$WORK_DIR/unexpand_wide.expected"
assert_files_equal "$WORK_DIR/unexpand_wide.expected" "$WORK_DIR/unexpand_wide.out" "unexpand did not use wide Unicode display columns before spaces"
