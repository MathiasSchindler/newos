#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

phase1_compiler_setup frontend

cat > "$WORK_DIR/sample.c" <<'EOF'
int main(void) {
    /* block comment coverage */
    // stage0 token coverage
    return 42;
}
EOF

"$ROOT_DIR/build/ncc" --dump-tokens "$WORK_DIR/sample.c" > "$WORK_DIR/tokens.out"
assert_file_contains "$WORK_DIR/tokens.out" 'keyword int' "compiler token stream missing int keyword"
assert_file_contains "$WORK_DIR/tokens.out" 'identifier main' "compiler token stream missing main identifier"
assert_file_contains "$WORK_DIR/tokens.out" 'number 42' "compiler token stream missing numeric literal"
assert_file_contains "$WORK_DIR/tokens.out" 'punct {' "compiler token stream missing punctuation"
assert_file_contains "$WORK_DIR/tokens.out" 'eof <eof>' "compiler token stream missing eof marker"

assert_command_succeeds "$ROOT_DIR/build/ncc" --target macos-aarch64 --dump-tokens "$WORK_DIR/sample.c" > /dev/null
"$ROOT_DIR/build/ncc" --dump-ast "$WORK_DIR/sample.c" > "$WORK_DIR/ast.out"
assert_file_contains "$WORK_DIR/ast.out" '^function main$' "compiler AST output missing main function"
"$ROOT_DIR/build/ncc" --dump-ir "$WORK_DIR/sample.c" > "$WORK_DIR/ir.out"
assert_file_contains "$WORK_DIR/ir.out" '^func main' "compiler IR output missing function header"
assert_file_contains "$WORK_DIR/ir.out" '^ret 42$' "compiler IR output missing return instruction"

cat > "$WORK_DIR/constant_fold.c" <<'EOF'
int main(void) {
    return (6 * 7) - (2 + 1);
}
EOF

"$ROOT_DIR/build/ncc" --dump-ir "$WORK_DIR/constant_fold.c" > "$WORK_DIR/constant_fold_ir.out"
assert_file_contains "$WORK_DIR/constant_fold_ir.out" '^ret 39$' "compiler IR optimizer did not fold a pure integer expression"

cat > "$WORK_DIR/constant_branch.c" <<'EOF'
int main(void) {
    if (1 < 2) {
        return 7;
    }
    return 9;
}
EOF

"$ROOT_DIR/build/ncc" --dump-ir "$WORK_DIR/constant_branch.c" > "$WORK_DIR/constant_branch_ir.out"
if grep -q '^brfalse ' "$WORK_DIR/constant_branch_ir.out"; then
    fail "compiler IR optimizer did not simplify a constant branch"
fi
if grep -q '^ret 9$' "$WORK_DIR/constant_branch_ir.out"; then
    fail "compiler IR optimizer did not prune unreachable IR after a constant branch"
fi

cat > "$WORK_DIR/identity_fold.c" <<'EOF'
int main(int argc, char **argv) {
    (void)argv;
    return ((argc + 0) ^ 0);
}
EOF

"$ROOT_DIR/build/ncc" --dump-ir "$WORK_DIR/identity_fold.c" > "$WORK_DIR/identity_fold_ir.out"
assert_file_contains "$WORK_DIR/identity_fold_ir.out" '^ret argc$' "compiler IR optimizer did not simplify neutral arithmetic identities"

cat > "$WORK_DIR/typedef_struct_local.c" <<'EOF'
typedef struct {
    unsigned char bytes[128];
} State;

int main(void) {
    State state;
    state.bytes[0] = 'x';
    return state.bytes[0] == 'x' ? 0 : 1;
}
EOF

"$ROOT_DIR/build/ncc" --dump-ir "$WORK_DIR/typedef_struct_local.c" > "$WORK_DIR/typedef_struct_local_ir.out"
assert_file_contains "$WORK_DIR/typedef_struct_local_ir.out" '^decl local obj struct state$' "compiler lost a typedef-backed struct local and lowered it as a plain scalar"

cat > "$WORK_DIR/local.h" <<'EOF'
#ifndef LOCAL_H
#define LOCAL_H
#define FEATURE_VALUE 7
#endif
EOF

cat > "$WORK_DIR/preprocess.c" <<'EOF'
#include "local.h"
#if defined(__APPLE__)
int platform_value = 1;
#else
int platform_value = 0;
#endif
int main(void) { return FEATURE_VALUE; }
EOF

"$ROOT_DIR/build/ncc" --preprocess --target macos-aarch64 "$WORK_DIR/preprocess.c" > "$WORK_DIR/preprocess.out"
assert_file_contains "$WORK_DIR/preprocess.out" 'int platform_value = 1;' "preprocessor did not keep the macOS branch"
assert_file_contains "$WORK_DIR/preprocess.out" 'return 7;' "preprocessor did not expand an object-like macro"
if grep -q 'platform_value = 0' "$WORK_DIR/preprocess.out"; then
    fail "preprocessor kept an inactive conditional branch"
fi

assert_command_succeeds "$ROOT_DIR/build/ncc" --dump-ast "$ROOT_DIR/src/tools/printf.c" > "$WORK_DIR/repo_ast.out"
repo_ast=$(tr -d '\r' < "$WORK_DIR/repo_ast.out")
case "$repo_ast" in
    *"function parse_signed_value"* ) ;;
    * ) fail "compiler parser did not accept repo source or missed function summary" ;;
esac

assert_command_succeeds "$ROOT_DIR/build/ncc" --dump-ast "$ROOT_DIR/src/tools/env.c" > "$WORK_DIR/repo_env_ast.out"
assert_file_contains "$WORK_DIR/repo_env_ast.out" '^function main$' "compiler parser did not accept env.c cleanly"
assert_command_succeeds "$ROOT_DIR/build/ncc" --dump-ast "$ROOT_DIR/src/tools/sh.c" > "$WORK_DIR/repo_sh_ast.out"
assert_file_contains "$WORK_DIR/repo_sh_ast.out" '^function sh_execute_pipeline$' "compiler parser did not accept sh.c cleanly"

cat > "$WORK_DIR/extern_redecl.c" <<'EOF'
extern int shared_value;
int shared_value;

int main(void) {
    extern int shared_value;
    return shared_value;
}
EOF

assert_command_succeeds "$ROOT_DIR/build/ncc" --dump-ast "$WORK_DIR/extern_redecl.c" > "$WORK_DIR/extern_redecl.out"
assert_file_contains "$WORK_DIR/extern_redecl.out" '^function main$' "compiler rejected a compatible extern redeclaration"

cat > "$WORK_DIR/for_scope.c" <<'EOF'
int main(void) {
    for (int i = 0; i < 2; i += 1) {
    }
    for (int i = 0; i < 2; i += 1) {
    }
    return 0;
}
EOF

assert_command_succeeds "$ROOT_DIR/build/ncc" --dump-ast "$WORK_DIR/for_scope.c" > "$WORK_DIR/for_scope.out"
assert_file_contains "$WORK_DIR/for_scope.out" '^function main$' "compiler did not keep for-loop declarations scoped to the loop"

cat > "$WORK_DIR/invalid.c" <<'EOF'
int main(void) {
    if (1 {
        return 0;
    }
}
EOF

if "$ROOT_DIR/build/ncc" --dump-ast "$WORK_DIR/invalid.c" > "$WORK_DIR/invalid.out" 2>&1; then
    fail "compiler parser unexpectedly accepted invalid syntax"
fi
assert_file_contains "$WORK_DIR/invalid.out" 'expected punctuation' "compiler parser missing syntax error message"

cat > "$WORK_DIR/semantic_error.c" <<'EOF'
int main(void) {
    int count = 1;
    int count = 2;
    return missing_value + count;
}
EOF

if "$ROOT_DIR/build/ncc" --dump-ast "$WORK_DIR/semantic_error.c" > "$WORK_DIR/semantic_error.out" 2>&1; then
    fail "compiler semantic analysis unexpectedly accepted duplicate or undeclared symbols"
fi
assert_file_contains "$WORK_DIR/semantic_error.out" 'duplicate declaration in the same scope\|use of undeclared identifier' "compiler semantic analysis missing symbol-table diagnostic"
