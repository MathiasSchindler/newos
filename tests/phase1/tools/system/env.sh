#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup env

FOO=phase1 "$ROOT_DIR/build/env" > "$WORK_DIR/env.out"
assert_file_contains "$WORK_DIR/env.out" '^FOO=phase1$' "env did not expose the injected variable"

env_cmd_out=$("$ROOT_DIR/build/env" -i FOO=solo "$ROOT_DIR/build/sh" -c 'printf %s "$FOO"' | tr -d '\r\n')
assert_text_equals "$env_cmd_out" 'solo' "env -i did not pass the requested clean environment"

env_unset_out=$(FOO=gone "$ROOT_DIR/build/env" -u FOO "$ROOT_DIR/build/sh" -c 'printf %s "${FOO-}"' | tr -d '\r\n')
assert_text_equals "$env_unset_out" '' "env -u did not remove the requested variable"
