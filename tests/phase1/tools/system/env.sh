#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup env

FOO=phase1 "${TEST_BIN_DIR}/env" > "$WORK_DIR/env.out"
assert_file_contains "$WORK_DIR/env.out" '^FOO=phase1$' "env did not expose the injected variable"

env_cmd_out=$("${TEST_BIN_DIR}/env" BAR=baz "${TEST_BIN_DIR}/sh" -c 'printf %s "$BAR"' | tr -d '\r\n')
assert_text_equals "$env_cmd_out" 'baz' "env did not pass variable overrides to the child command"

env_cmd_out=$("${TEST_BIN_DIR}/env" -i FOO=solo "${TEST_BIN_DIR}/sh" -c 'printf %s "$FOO"' | tr -d '\r\n')
assert_text_equals "$env_cmd_out" 'solo' "env -i did not pass the requested clean environment"

env_unset_out=$(FOO=gone "${TEST_BIN_DIR}/env" -u FOO "${TEST_BIN_DIR}/sh" -c 'printf %s "${FOO-}"' | tr -d '\r\n')
assert_text_equals "$env_unset_out" '' "env -u did not remove the requested variable"

env_zero_out=$("${TEST_BIN_DIR}/env" -0 FOO=bar | tr '\0' '\n' | grep '^FOO=' | tr -d '\r\n')
assert_text_equals "$env_zero_out" 'FOO=bar' "env -0 did not emit NUL-delimited output"
