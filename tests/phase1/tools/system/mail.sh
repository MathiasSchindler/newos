#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup mail

for mode in interactive --list; do
    for syntax in colon missing_equals; do
        if [ "$syntax" = colon ]; then
            printf 'imap.host: localhost\n' > "$WORK_DIR/config"
            config_diagnostic="expected key=value, found ':' instead of '='"
        else
            printf 'imap.host localhost\n' > "$WORK_DIR/config"
            config_diagnostic="expected key=value"
        fi
        status=0
        if [ "$mode" = interactive ]; then
            "${TEST_BIN_DIR}/mail" "$WORK_DIR/config" > "$WORK_DIR/out" 2> "$WORK_DIR/err" || status=$?
        else
            "${TEST_BIN_DIR}/mail" "$mode" "$WORK_DIR/config" > "$WORK_DIR/out" 2> "$WORK_DIR/err" || status=$?
        fi
        assert_exit_code "$status" 1 "mail should reject malformed config"
        grep -Fx "mail: config error on line 1: $config_diagnostic" "$WORK_DIR/err" >/dev/null ||
            fail "mail did not preserve the $syntax config diagnostic in $mode mode"
    done
done
