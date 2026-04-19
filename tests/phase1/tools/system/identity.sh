#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup identity

whoami_out=$("$ROOT_DIR/build/whoami" | tr -d '\r\n')
assert_nonempty_text "$whoami_out" "whoami output was empty"

id_user_out=$("$ROOT_DIR/build/id" -un | tr -d '\r\n')
assert_text_equals "$id_user_out" "$whoami_out" "id -un and whoami disagreed"

"$ROOT_DIR/build/id" > "$WORK_DIR/id.out"
assert_file_contains "$WORK_DIR/id.out" '^uid=[0-9]' "id summary did not include a uid"
assert_file_contains "$WORK_DIR/id.out" "($whoami_out)" "id summary did not mention the current user"

numeric_groups_out=$("$ROOT_DIR/build/id" -G | tr -d '\r\n')
assert_nonempty_text "$numeric_groups_out" "id -G returned no group identifiers"

groups_out=$("$ROOT_DIR/build/groups" | tr -d '\r\n')
assert_nonempty_text "$groups_out" "groups output was empty"
