#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir tsort)

note "phase1 text: tsort"

cat > "$WORK_DIR/pairs.txt" <<'EOF'
base lib
lib app
base docs
EOF

"$ROOT_DIR/build/tsort" "$WORK_DIR/pairs.txt" > "$WORK_DIR/order.out"
base_line=$(grep -n '^base$' "$WORK_DIR/order.out" | cut -d: -f1)
lib_line=$(grep -n '^lib$' "$WORK_DIR/order.out" | cut -d: -f1)
app_line=$(grep -n '^app$' "$WORK_DIR/order.out" | cut -d: -f1)
docs_line=$(grep -n '^docs$' "$WORK_DIR/order.out" | cut -d: -f1)
[ -n "$base_line" ] && [ -n "$lib_line" ] && [ -n "$app_line" ] && [ -n "$docs_line" ] || fail "tsort output missed expected nodes"
[ "$base_line" -lt "$lib_line" ] || fail "tsort did not place base before lib"
[ "$lib_line" -lt "$app_line" ] || fail "tsort did not place lib before app"
[ "$base_line" -lt "$docs_line" ] || fail "tsort did not place base before docs"

cycle_status=0
printf 'a b\nb a\n' | "$ROOT_DIR/build/tsort" > "$WORK_DIR/cycle.out" 2> "$WORK_DIR/cycle.err" || cycle_status=$?
[ "$cycle_status" -ne 0 ] || fail "tsort should reject a dependency cycle"
assert_file_contains "$WORK_DIR/cycle.err" 'cycle' "tsort cycle diagnostic did not mention the cycle"