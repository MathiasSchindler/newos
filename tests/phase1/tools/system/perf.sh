#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup perf

if [ ! -x "${TEST_BIN_DIR}/perf" ]; then
	note "perf binary is not present for this target; skipping"
	exit 0
fi

assert_command_succeeds "${TEST_BIN_DIR}/perf" --help > "$WORK_DIR/help.out" 2> "$WORK_DIR/help.err"
assert_file_contains "$WORK_DIR/help.err" '^Usage: perf ' "perf help output changed"

os_name=$(uname -s 2>/dev/null || echo unknown)
if [ "$os_name" != Linux ]; then
	status=0
	"${TEST_BIN_DIR}/perf" -- /bin/true > "$WORK_DIR/nonlinux.out" 2> "$WORK_DIR/nonlinux.err" || status=$?
	assert_exit_code "$status" 125 "perf should report unsupported non-Linux targets"
	assert_file_contains "$WORK_DIR/nonlinux.err" 'perf_event_open support is required' "perf did not explain non-Linux limitation"
	exit 0
fi

status=0
"${TEST_BIN_DIR}/perf" -n 5 -F 99 -- /bin/sh -c 'i=0; while [ "$i" -lt 200000 ]; do i=$((i + 1)); done' > "$WORK_DIR/run.out" 2> "$WORK_DIR/run.err" || status=$?

case "$status" in
	0)
		assert_file_contains "$WORK_DIR/run.out" '^samples=[0-9][0-9]* lost=[0-9][0-9]* elapsed_ms=[0-9][0-9]* exit=0$' "perf summary output changed"
		assert_file_contains "$WORK_DIR/run.out" '^rank samples pct address function$' "perf table header changed"
		;;
	125)
		assert_file_contains "$WORK_DIR/run.err" 'perf_event_open failed' "perf permission path did not mention perf_event_open"
		assert_file_contains "$WORK_DIR/run.err" 'kernel.perf_event_paranoid' "perf permission path did not mention perf_event_paranoid"
		;;
	*)
		cat "$WORK_DIR/run.out" >&2 || true
		cat "$WORK_DIR/run.err" >&2 || true
		fail "perf profiling smoke returned unexpected status $status"
		;;
esac
