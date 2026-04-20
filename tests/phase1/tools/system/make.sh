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

cat > "$WORK_DIR/Export.mk" <<'EOF'
export MSG = exported-value
all:
	printf '%s\n' "$$MSG" > exported.txt
EOF
(
    cd "$WORK_DIR"
    assert_command_succeeds "$ROOT_DIR/build/make" -f Export.mk
)
assert_file_contains "$WORK_DIR/exported.txt" '^exported-value$' "make export directives did not reach the recipe environment"

cat > "$WORK_DIR/Force.mk" <<'EOF'
all:
	+printf 'forced\n' > forced.txt
EOF
(
    cd "$WORK_DIR"
    assert_command_succeeds "$ROOT_DIR/build/make" -n -f Force.mk > force.out
)
assert_file_contains "$WORK_DIR/force.out" "printf 'forced\\\\n' > forced.txt" "make -n did not still echo a forced recipe"
assert_file_contains "$WORK_DIR/forced.txt" '^forced$' "make did not execute a '+' recipe under -n"

mkdir -p "$WORK_DIR/oneshell/sub"
cat > "$WORK_DIR/OneShell.mk" <<'EOF'
.ONESHELL:
all:
	cd oneshell/sub
	printf 'persisted\n' > marker.txt
EOF
(
    cd "$WORK_DIR"
    assert_command_succeeds "$ROOT_DIR/build/make" -f OneShell.mk
)
assert_file_contains "$WORK_DIR/oneshell/sub/marker.txt" '^persisted$' "make .ONESHELL did not keep shell state across recipe lines"

cat > "$WORK_DIR/Parallel.mk" <<'EOF'
all: one two

one:
	sleep 2
	printf 'one\n' > one

two:
	sleep 2
	printf 'two\n' > two
EOF
(
    cd "$WORK_DIR"
    start=$(date +%s)
    assert_command_succeeds "$ROOT_DIR/build/make" -f Parallel.mk -j2 > parallel.out
    end=$(date +%s)
    elapsed=$((end - start))
    [ "$elapsed" -lt 4 ] || fail "make -j2 did not overlap independent targets"
)
assert_file_contains "$WORK_DIR/one" '^one$' "make -j2 did not build the first target output"
assert_file_contains "$WORK_DIR/two" '^two$' "make -j2 did not build the second target output"

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
