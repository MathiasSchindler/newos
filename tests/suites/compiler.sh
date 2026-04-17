#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"

WORK_DIR="$ROOT_DIR/tests/tmp/compiler"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

note "compiler"

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

"$ROOT_DIR/build/ncc" --target macos-aarch64 --dump-tokens "$WORK_DIR/sample.c" > /dev/null || fail "compiler did not accept macos-aarch64 target"
"$ROOT_DIR/build/ncc" --dump-ast "$WORK_DIR/sample.c" > "$WORK_DIR/ast.out"
assert_file_contains "$WORK_DIR/ast.out" '^function main$' "compiler AST output missing main function"
"$ROOT_DIR/build/ncc" --dump-ir "$WORK_DIR/sample.c" > "$WORK_DIR/ir.out"
assert_file_contains "$WORK_DIR/ir.out" '^func main' "compiler IR output missing function header"
assert_file_contains "$WORK_DIR/ir.out" '^ret 42$' "compiler IR output missing return instruction"
"$ROOT_DIR/build/ncc" -S --target linux-x86_64 "$WORK_DIR/sample.c" > "$WORK_DIR/sample.s"
assert_file_contains "$WORK_DIR/sample.s" '^\.globl main$' "compiler x86_64 backend missing global symbol"
assert_file_contains "$WORK_DIR/sample.s" 'movq \$42, %rax' "compiler x86_64 backend missing immediate return code"
"$ROOT_DIR/build/ncc" -S --target macos-aarch64 "$WORK_DIR/sample.c" -o "$WORK_DIR/sample_macos.s"
assert_file_contains "$WORK_DIR/sample_macos.s" '^\.globl _main$' "compiler macOS AArch64 backend missing Darwin global symbol"
assert_file_contains "$WORK_DIR/sample_macos.s" 'movz x0, #42' "compiler macOS AArch64 backend missing immediate return code"
"$ROOT_DIR/build/ncc" -c --target macos-aarch64 "$WORK_DIR/sample.c" -o "$WORK_DIR/sample_macos.o"
"$ROOT_DIR/build/hexdump" "$WORK_DIR/sample_macos.o" > "$WORK_DIR/sample_macos_obj_hex.out"
assert_file_contains "$WORK_DIR/sample_macos_obj_hex.out" 'cf fa ed fe' "compiler macOS object writer did not emit Mach-O magic"
"$ROOT_DIR/build/ncc" --target macos-aarch64 "$WORK_DIR/sample.c" -o "$WORK_DIR/sample_macos_bin"
if "$WORK_DIR/sample_macos_bin"; then
    sample_status=0
else
    sample_status=$?
fi
assert_text_equals "$sample_status" "42" "compiler macOS linker did not produce a runnable executable"
"$ROOT_DIR/build/ncc" -c --target linux-x86_64 "$WORK_DIR/sample.c" -o "$WORK_DIR/sample.o"
"$ROOT_DIR/build/hexdump" "$WORK_DIR/sample.o" > "$WORK_DIR/sample_obj_hex.out"
assert_file_contains "$WORK_DIR/sample_obj_hex.out" '7f 45 4c 46' "compiler object writer did not emit ELF magic"

cat > "$WORK_DIR/flow.c" <<'EOF'
int main(void) {
    int value = 3;
    if (value < 5) {
        value = value + 1;
    } else {
        value = value - 1;
    }
    return value;
}
EOF

"$ROOT_DIR/build/ncc" -S --target linux-x86_64 "$WORK_DIR/flow.c" > "$WORK_DIR/flow.s"
assert_file_contains "$WORK_DIR/flow.s" '^\.Lelse[0-9][0-9]*:$' "compiler backend missing conditional branch label"
assert_file_contains "$WORK_DIR/flow.s" 'addq %rcx, %rax' "compiler backend missing arithmetic lowering"
"$ROOT_DIR/build/ncc" -S --target macos-aarch64 "$WORK_DIR/flow.c" -o "$WORK_DIR/flow_macos.s"
assert_file_contains "$WORK_DIR/flow_macos.s" 'b\.eq \.Lelse[0-9][0-9]*' "compiler macOS AArch64 backend missing conditional branch"
assert_file_contains "$WORK_DIR/flow_macos.s" 'add x0, x1, x2' "compiler macOS AArch64 backend missing arithmetic lowering"
"$ROOT_DIR/build/ncc" -c --target macos-aarch64 "$WORK_DIR/flow.c" -o "$WORK_DIR/flow_macos.o"
"$ROOT_DIR/build/hexdump" "$WORK_DIR/flow_macos.o" > "$WORK_DIR/flow_macos_obj_hex.out"
assert_file_contains "$WORK_DIR/flow_macos_obj_hex.out" 'cf fa ed fe' "compiler macOS object writer did not handle control-flow object emission"
"$ROOT_DIR/build/ncc" --target macos-aarch64 "$WORK_DIR/flow.c" -o "$WORK_DIR/flow_macos_bin"
if "$WORK_DIR/flow_macos_bin"; then
    flow_status=0
else
    flow_status=$?
fi
assert_text_equals "$flow_status" "4" "compiler macOS linker did not preserve control-flow semantics"
"$ROOT_DIR/build/ncc" -c --target linux-x86_64 "$WORK_DIR/flow.c" -o "$WORK_DIR/flow.o"
"$ROOT_DIR/build/hexdump" "$WORK_DIR/flow.o" > "$WORK_DIR/flow_obj_hex.out"
assert_file_contains "$WORK_DIR/flow_obj_hex.out" '7f 45 4c 46' "compiler object writer did not handle control-flow object emission"

cat > "$WORK_DIR/backend_expr.c" <<'EOF'
int main(void) {
    char buffer[16];
    buffer[0] = "ok"[0];
    buffer[1] = "ok"[1];
    buffer[2] = '\0';
    return buffer[1] == 'k' ? 0 : 1;
}
EOF

"$ROOT_DIR/build/ncc" -S --target macos-aarch64 "$WORK_DIR/backend_expr.c" -o "$WORK_DIR/backend_expr_macos.s"
assert_file_contains "$WORK_DIR/backend_expr_macos.s" '^\.Lstr[0-9][0-9]*:$' "compiler backend missing string literal emission"
"$ROOT_DIR/build/ncc" --target macos-aarch64 "$WORK_DIR/backend_expr.c" -o "$WORK_DIR/backend_expr_macos_bin"
if "$WORK_DIR/backend_expr_macos_bin"; then
    backend_expr_status=0
else
    backend_expr_status=$?
fi
assert_text_equals "$backend_expr_status" "0" "compiler backend did not preserve string/index expression semantics"

"$ROOT_DIR/build/ncc" -S --target macos-aarch64 "$ROOT_DIR/src/tools/pwd.c" -o "$WORK_DIR/pwd_repo.s"
"$ROOT_DIR/build/ncc" -S --target macos-aarch64 "$ROOT_DIR/src/tools/echo.c" -o "$WORK_DIR/echo_repo.s"
"$ROOT_DIR/build/ncc" -S --target macos-aarch64 "$ROOT_DIR/src/tools/basename.c" -o "$WORK_DIR/basename_repo.s"

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

"$ROOT_DIR/build/ncc" -c "$WORK_DIR/sample.c" -o "$WORK_DIR/default_host.o"
"$ROOT_DIR/build/hexdump" "$WORK_DIR/default_host.o" > "$WORK_DIR/default_host_obj_hex.out"
if ! grep -q '7f 45 4c 46' "$WORK_DIR/default_host_obj_hex.out" && ! grep -q 'cf fa ed fe' "$WORK_DIR/default_host_obj_hex.out"; then
    fail "compiler default target did not emit a supported object format"
fi
"$ROOT_DIR/build/ncc" "$WORK_DIR/sample.c" -o "$WORK_DIR/default_host_bin"
if "$WORK_DIR/default_host_bin"; then
    default_link_status=0
else
    default_link_status=$?
fi
assert_text_equals "$default_link_status" "42" "compiler default target did not link a runnable executable"
