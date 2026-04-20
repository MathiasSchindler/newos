#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

phase1_compiler_setup codegen

cat > "$WORK_DIR/sample.c" <<'EOF'
int main(void) {
    return 42;
}
EOF

"$ROOT_DIR/build/ncc" -S --target linux-x86_64 "$WORK_DIR/sample.c" -o "$WORK_DIR/sample_linux.s"
assert_file_contains "$WORK_DIR/sample_linux.s" '^\.globl main$' "compiler x86_64 backend missing global symbol"
assert_file_contains "$WORK_DIR/sample_linux.s" 'movq \$42, %rax' "compiler x86_64 backend missing immediate return code"

"$ROOT_DIR/build/ncc" -S --target macos-aarch64 "$WORK_DIR/sample.c" -o "$WORK_DIR/sample_macos.s"
assert_file_contains "$WORK_DIR/sample_macos.s" '^\.globl _main$' "compiler macOS AArch64 backend missing Darwin global symbol"
assert_file_contains "$WORK_DIR/sample_macos.s" 'movz x0, #42' "compiler macOS AArch64 backend missing immediate return code"
assert_command_succeeds "$ROOT_DIR/build/ncc" -Wno-pedantic -S --target macos-aarch64 "$WORK_DIR/sample.c" -o "$WORK_DIR/sample_warn_macos.s"

"$ROOT_DIR/build/ncc" -c --target linux-x86_64 "$WORK_DIR/sample.c" -o "$WORK_DIR/sample_linux.o"
"$ROOT_DIR/build/hexdump" "$WORK_DIR/sample_linux.o" > "$WORK_DIR/sample_linux_hex.out"
assert_file_contains "$WORK_DIR/sample_linux_hex.out" '7f 45 4c 46' "compiler object writer did not emit ELF magic"

"$ROOT_DIR/build/ncc" -c --target macos-aarch64 "$WORK_DIR/sample.c" -o "$WORK_DIR/sample_macos.o"
"$ROOT_DIR/build/hexdump" "$WORK_DIR/sample_macos.o" > "$WORK_DIR/sample_macos_hex.out"
assert_file_contains "$WORK_DIR/sample_macos_hex.out" 'cf fa ed fe' "compiler macOS object writer did not emit Mach-O magic"

cat > "$WORK_DIR/flow.c" <<'EOF'
int adjust(int value) {
    if (value < 5) {
        value = value + 1;
    } else {
        value = value - 1;
    }
    return value;
}

int main(void) {
    return adjust(3);
}
EOF

"$ROOT_DIR/build/ncc" -S --target linux-x86_64 "$WORK_DIR/flow.c" -o "$WORK_DIR/flow_linux.s"
assert_file_contains "$WORK_DIR/flow_linux.s" '^\.L[a-zA-Z0-9_]*_else[0-9][0-9]*:$' "compiler backend missing conditional branch label"
assert_file_contains "$WORK_DIR/flow_linux.s" 'addq %rcx, %rax' "compiler backend missing arithmetic lowering"

"$ROOT_DIR/build/ncc" -S --target macos-aarch64 "$WORK_DIR/flow.c" -o "$WORK_DIR/flow_macos.s"
assert_file_contains "$WORK_DIR/flow_macos.s" 'b\.eq \.L[a-zA-Z0-9_]*_else[0-9][0-9]*' "compiler macOS AArch64 backend missing conditional branch"
assert_file_contains "$WORK_DIR/flow_macos.s" 'add x0, x1, x2' "compiler macOS AArch64 backend missing arithmetic lowering"

"$ROOT_DIR/build/ncc" -c --target macos-aarch64 "$WORK_DIR/flow.c" -o "$WORK_DIR/flow_macos.o"
"$ROOT_DIR/build/hexdump" "$WORK_DIR/flow_macos.o" > "$WORK_DIR/flow_macos_hex.out"
assert_file_contains "$WORK_DIR/flow_macos_hex.out" 'cf fa ed fe' "compiler macOS object writer did not handle control-flow object emission"

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

cat > "$WORK_DIR/call_index_expr.c" <<'EOF'
const char *pick(void) {
    return "ok";
}

int main(void) {
    return pick()[1] == 'k' ? 0 : 1;
}
EOF

assert_command_succeeds "$ROOT_DIR/build/ncc" -S --target macos-aarch64 "$WORK_DIR/call_index_expr.c" -o "$WORK_DIR/call_index_expr_macos.s"
assert_file_contains "$WORK_DIR/call_index_expr_macos.s" 'bl _pick' "compiler backend did not preserve postfix indexing after a function call"

cat > "$WORK_DIR/extern_data.c" <<'EOF'
extern char **environ;

int main(void) {
    return environ != 0 ? 0 : 1;
}
EOF

"$ROOT_DIR/build/ncc" -S --target macos-aarch64 "$WORK_DIR/extern_data.c" -o "$WORK_DIR/extern_data_macos.s"
assert_file_contains "$WORK_DIR/extern_data_macos.s" '@GOTPAGE' "compiler macOS backend missing GOT-based global data access"

cat > "$WORK_DIR/long_expr.c" <<'EOF'
int main(void) {
    return 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1;
}
EOF

assert_command_succeeds "$ROOT_DIR/build/ncc" -c --target macos-aarch64 "$WORK_DIR/long_expr.c" -o "$WORK_DIR/long_expr_macos.o"

"$ROOT_DIR/build/ncc" -S --target macos-aarch64 "$ROOT_DIR/src/tools/pwd.c" -o "$WORK_DIR/pwd_repo.s"
"$ROOT_DIR/build/ncc" -S --target macos-aarch64 "$ROOT_DIR/src/tools/echo.c" -o "$WORK_DIR/echo_repo.s"
"$ROOT_DIR/build/ncc" -S --target macos-aarch64 "$ROOT_DIR/src/tools/basename.c" -o "$WORK_DIR/basename_repo.s"

assert_command_succeeds "$ROOT_DIR/build/ncc" -c --target linux-x86_64 "$ROOT_DIR/src/shared/runtime/unicode.c" -o "$WORK_DIR/unicode_repo.o"
"$ROOT_DIR/build/hexdump" "$WORK_DIR/unicode_repo.o" > "$WORK_DIR/unicode_repo_hex.out"
assert_file_contains "$WORK_DIR/unicode_repo_hex.out" '7f 45 4c 46' "compiler failed on a large repository-scale Unicode initializer"
