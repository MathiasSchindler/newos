#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup make

printf 'MSG ?= fallback\nall:\n\tprintf '\''%%s\\n'\'' $(MSG) > out.txt\n' > "$WORK_DIR/Makefile"

(
    cd "$WORK_DIR"
    assert_command_succeeds "$ROOT_DIR/build/make" MSG=phase1
)
assert_file_contains "$WORK_DIR/out.txt" '^phase1$' "make did not apply a command-line variable override"

(
    cd "$WORK_DIR"
    assert_command_succeeds "$ROOT_DIR/build/make" -n MSG=dry-run > dry.out
)
assert_file_contains "$WORK_DIR/dry.out" "printf '%s\\\\n' dry-run > out.txt" "make -n did not print the recipe without executing it"

"$ROOT_DIR/build/make" --color=always --help > "$WORK_DIR/help.out" 2>&1
if ! LC_ALL=C grep -q "$(printf '\033')\\[" "$WORK_DIR/help.out"; then
    fail "make --color=always --help did not emit ANSI color sequences"
fi

printf 'all:\n\tprintf '\''%%s\\n'\'' "$(MAKEFLAGS)" > flags.txt\n' > "$WORK_DIR/Flags.mk"
(
    cd "$WORK_DIR"
    assert_command_succeeds "$ROOT_DIR/build/make" -f Flags.mk -j4
)
assert_file_contains "$WORK_DIR/flags.txt" 'j4' "make did not expose the requested job count via MAKEFLAGS"

printf 'all:\n\tprintf '\''built\\n'\'' > silent.txt\n' > "$WORK_DIR/Silent.mk"
(
    cd "$WORK_DIR"
    assert_command_succeeds "$ROOT_DIR/build/make" -s -f Silent.mk > silent.out
)
silent_out=$(tr -d '\r\n' < "$WORK_DIR/silent.out")
assert_text_equals "$silent_out" '' "make -s unexpectedly echoed the recipe"
assert_file_contains "$WORK_DIR/silent.txt" '^built$' "make -s did not still execute the recipe"

cat > "$WORK_DIR/Always.mk" <<'EOF'
all: stamp.txt

stamp.txt:
	printf 'run\n' >> stamp.txt
EOF
(
    cd "$WORK_DIR"
    assert_command_succeeds "$ROOT_DIR/build/make" -f Always.mk
    assert_command_succeeds "$ROOT_DIR/build/make" -B -f Always.mk
)
always_runs=$(wc -l < "$WORK_DIR/stamp.txt" | tr -d '[:space:]')
assert_text_equals "$always_runs" '2' "make -B did not force the target recipe to run again"
