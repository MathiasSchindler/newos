#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup profiler

cat > "$WORK_DIR/macos_slide.nprof" <<'EOF'
enter 0 0x100101000
enter 10 0x100102000
exit 40 0x100102000
enter 45 0x100103000
exit 55 0x100103000
exit 100 0x100101000
EOF

cat > "$WORK_DIR/macos_slide.map" <<'EOF'
symbol 0x0000000100001000 96 __TEXT,__text _main src/tools/example.o
symbol 0x0000000100002000 64 __TEXT,__text _parse_inner src/tools/example.o
symbol 0x0000000100003000 32 __TEXT,__text _emit_result src/tools/example.o
EOF

assert_command_succeeds "${TEST_BIN_DIR}/profiler" --csv --sort total -n 0 -m "$WORK_DIR/macos_slide.map" "$WORK_DIR/macos_slide.nprof" > "$WORK_DIR/macos_slide.csv" 2> "$WORK_DIR/macos_slide.err"
assert_file_contains "$WORK_DIR/macos_slide.csv" '^rank,calls,self_ns,total_ns,max_ns,self_pct,total_pct,avg_self_ns,avg_total_ns,address,function$' "profiler CSV header changed"
assert_file_contains "$WORK_DIR/macos_slide.csv" '^1,1,60,100,100,60\.00,100\.00,60,100,0x100101000,main$' "profiler did not account and symbolize slid main from Mach-O map"
assert_file_contains "$WORK_DIR/macos_slide.csv" '^2,1,30,30,30,30\.00,30\.00,30,30,0x100102000,parse_inner$' "profiler did not account nested function self time"
assert_file_contains "$WORK_DIR/macos_slide.csv" '^3,1,10,10,10,10\.00,10\.00,10,10,0x100103000,emit_result$' "profiler did not keep the third slid Mach-O symbol"

cat > "$WORK_DIR/aliases.nprof" <<'EOF'
+ 0 0x4000
e 5 0x5000
x 8 0x5000
- 10 0x4000
EOF

cat > "$WORK_DIR/aliases.syms" <<'EOF'
0x4000 outer
0x5000 inner
EOF

assert_command_succeeds "${TEST_BIN_DIR}/profiler" --csv --sort addr -m "$WORK_DIR/aliases.syms" --write-call-graph-profile "$WORK_DIR/aliases.cgprofile" "$WORK_DIR/aliases.nprof" > "$WORK_DIR/aliases.csv" 2> "$WORK_DIR/aliases.err"
assert_file_contains "$WORK_DIR/aliases.csv" '^1,1,7,10,10,70\.00,100\.00,7,10,0x4000,outer$' "profiler did not parse + and - trace aliases"
assert_file_contains "$WORK_DIR/aliases.csv" '^2,1,3,3,3,30\.00,30\.00,3,3,0x5000,inner$' "profiler did not parse e and x trace aliases"
assert_file_contains "$WORK_DIR/aliases.cgprofile" '^node 10 outer$' "profiler call graph profile omitted outer node weight"
assert_file_contains "$WORK_DIR/aliases.cgprofile" '^node 3 inner$' "profiler call graph profile omitted inner node weight"
assert_file_contains "$WORK_DIR/aliases.cgprofile" '^edge 1 outer inner$' "profiler call graph profile omitted caller/callee weight"

if [ "$(uname -s 2>/dev/null || echo unknown)" = Linux ] && [ "$(uname -m 2>/dev/null || echo unknown)" = x86_64 ]; then
	PROFILE_BUILD_DIR="$WORK_DIR/profile-build"
	if ! make -C "$ROOT_DIR" freestanding TOOLS='profiler cat' PROFILE=1 LINKER_REPORTS=1 TARGET_BUILD_DIR="$PROFILE_BUILD_DIR" > "$WORK_DIR/profile-build.log" 2>&1; then
		sed -n '1,160p' "$WORK_DIR/profile-build.log" >&2
		fail "profiler Linux PROFILE=1 freestanding smoke build failed"
	fi
	assert_command_succeeds env NEWOS_PROFILE="$WORK_DIR/cat.nprof" "$PROFILE_BUILD_DIR/cat" "$ROOT_DIR/README.md" > "$WORK_DIR/profile-cat.stdout" 2> "$WORK_DIR/profile-cat.stderr"
	assert_command_succeeds "$PROFILE_BUILD_DIR/profiler" --csv --sort total -m "$PROFILE_BUILD_DIR/.maps/cat.map" "$WORK_DIR/cat.nprof" > "$WORK_DIR/profile-cat.csv" 2> "$WORK_DIR/profile-cat.err"
	assert_file_contains "$WORK_DIR/cat.nprof" '^enter [0-9][0-9]* [0-9][0-9]* 0x[0-9a-f][0-9a-f]*$' "Linux PROFILE=1 runtime did not emit enter events"
	assert_file_contains "$WORK_DIR/profile-cat.csv" ',main$' "profiler did not symbolize Linux newlinker map main"
	assert_file_contains "$WORK_DIR/profile-cat.csv" ',cat_' "profiler did not symbolize Linux newlinker text-section functions"
fi