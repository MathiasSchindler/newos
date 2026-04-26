#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup patch

printf 'alpha\nbeta\n' > "$WORK_DIR/target.txt"
cat > "$WORK_DIR/change.diff" <<'EOF'
--- a/target.txt
+++ b/target.txt
@@ -1,2 +1,2 @@
 alpha
-beta
+gamma
EOF

(
    cd "$WORK_DIR"
    "$ROOT_DIR/build/patch" -p1 --dry-run -i change.diff > dry.out
)
assert_file_contains "$WORK_DIR/dry.out" '^checked ' "patch --dry-run did not validate the hunk"
assert_file_contains "$WORK_DIR/target.txt" '^beta$' "patch --dry-run modified the target"

(
    cd "$WORK_DIR"
    "$ROOT_DIR/build/patch" -p1 -b -i change.diff > apply.out
)
assert_file_contains "$WORK_DIR/target.txt" '^gamma$' "patch did not apply the hunk"
assert_file_contains "$WORK_DIR/target.txt.orig" '^beta$' "patch -b did not keep an original backup"

(
    cd "$WORK_DIR"
    "$ROOT_DIR/build/patch" -R -p1 -i change.diff > reverse.out
)
assert_file_contains "$WORK_DIR/target.txt" '^beta$' "patch -R did not reverse the hunk"