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
assert_file_contains "$WORK_DIR/sample_linux.s" 'pushq \$42' "compiler x86_64 backend missing compact immediate return code"
assert_file_contains "$WORK_DIR/sample_linux.s" 'popq %rax' "compiler x86_64 backend missing immediate return register load"

cat > "$WORK_DIR/zero_branch.c" <<'EOF'
int main(int argc, char **argv) {
    (void)argv;
    if (argc) {
        return 1;
    }
    return 0;
}
EOF

"$ROOT_DIR/build/ncc" -S --target linux-x86_64 "$WORK_DIR/zero_branch.c" -o "$WORK_DIR/zero_branch_linux.s"
assert_file_contains "$WORK_DIR/zero_branch_linux.s" 'testq %rax, %rax' "compiler x86_64 backend should use compact zero tests"

cat > "$WORK_DIR/cached_params.c" <<'EOF'
int main(int argc, char **argv) {
    int total = 0;
    if (argc > 2) {
        total = total + argv[0][0];
        total = total + argv[1][0];
        total = total + argv[argc - 1][0];
        return total + argc;
    }
    return argc;
}
EOF

"$ROOT_DIR/build/ncc" -S --target linux-x86_64 "$WORK_DIR/cached_params.c" -o "$WORK_DIR/cached_params_linux.s"
assert_file_contains "$WORK_DIR/cached_params_linux.s" 'movq %rdi, %rbx' "compiler x86_64 backend should cache immutable first parameter"
assert_file_contains "$WORK_DIR/cached_params_linux.s" 'movq %rsi, %r12' "compiler x86_64 backend should cache immutable second parameter"
assert_file_contains "$WORK_DIR/cached_params_linux.s" 'movslq %ebx, %rax' "compiler x86_64 backend should sign-extend cached int parameters"

cat > "$WORK_DIR/mutated_param.c" <<'EOF'
int main(int argc, char **argv) {
    (void)argv;
    argc = argc + 1;
    return argc;
}
EOF

"$ROOT_DIR/build/ncc" -S --target linux-x86_64 "$WORK_DIR/mutated_param.c" -o "$WORK_DIR/mutated_param_linux.s"
if grep -q 'movq %rdi, %rbx' "$WORK_DIR/mutated_param_linux.s"; then
    fail "compiler x86_64 backend should not cache mutated parameters"
fi

cat > "$WORK_DIR/cached_local.c" <<'EOF'
int main(int argc, char **argv) {
    int base = argc + 1;
    (void)argv;
    if (base > 10) {
        return base;
    }
    if (base < 0) {
        return base + 1;
    }
    return base + 2;
}
EOF

"$ROOT_DIR/build/ncc" -S --target linux-x86_64 "$WORK_DIR/cached_local.c" -o "$WORK_DIR/cached_local_linux.s"
assert_file_contains "$WORK_DIR/cached_local_linux.s" 'movq %rax, %rbx' "compiler x86_64 backend should cache single-assignment scalar locals"
assert_file_contains "$WORK_DIR/cached_local_linux.s" 'movslq %ebx, %rax' "compiler x86_64 backend should sign-extend cached int locals"

cat > "$WORK_DIR/cached_mutable_local.c" <<'EOF'
int main(int argc, char **argv) {
    int i = 1;
    (void)argv;
    if (i < argc) {
        ++i;
    }
    if (i < argc) {
        return i;
    }
    return i + 1;
}
EOF

"$ROOT_DIR/build/ncc" -S --target linux-x86_64 "$WORK_DIR/cached_mutable_local.c" -o "$WORK_DIR/cached_mutable_local_linux.s"
assert_file_contains "$WORK_DIR/cached_mutable_local_linux.s" 'addq \$1, %rbx' "compiler x86_64 backend should update cached mutable locals in registers"
assert_file_contains "$WORK_DIR/cached_mutable_local_linux.s" 'movslq %ebx, %rax' "compiler x86_64 backend should read cached mutable int locals from registers"
compile_and_check_native "$WORK_DIR/cached_mutable_local.c" "$WORK_DIR/cached_mutable_local_bin" 2 "compiler x86_64 backend should preserve cached mutable local semantics"

cat > "$WORK_DIR/cached_index.c" <<'EOF'
int main(int argc, char **argv) {
    int i = 0;
    if (argc > 1) {
        ++i;
    }
    if (i < argc) {
        return argv[i][0];
    }
    return i;
}
EOF

"$ROOT_DIR/build/ncc" -S --target linux-x86_64 "$WORK_DIR/cached_index.c" -o "$WORK_DIR/cached_index_linux.s"
assert_file_contains "$WORK_DIR/cached_index_linux.s" 'movslq %ebx, %r11' "compiler x86_64 backend should extend cached int indexes from registers"
assert_file_contains "$WORK_DIR/cached_index_linux.s" 'leaq (%rax,%r11,8), %rax' "compiler x86_64 backend should address cached-index pointer loads directly"

cat > "$WORK_DIR/direct_branch_compare.c" <<'EOF'
int main(int argc, char **argv) {
    int i = 1;
    (void)argv;
    if (i < argc) {
        return 2;
    }
    if (i > 1) {
        return 3;
    }
    if (argc > 1) {
        return 4;
    }
    return i;
}
EOF

"$ROOT_DIR/build/ncc" -S --target linux-x86_64 "$WORK_DIR/direct_branch_compare.c" -o "$WORK_DIR/direct_branch_compare_linux.s"
assert_file_contains "$WORK_DIR/direct_branch_compare_linux.s" 'movslq (%rax), %rcx' "compiler x86_64 backend should load simple branch RHS directly"
assert_file_contains "$WORK_DIR/direct_branch_compare_linux.s" 'cmpq %rcx, %rax' "compiler x86_64 backend should compare simple branch operands without expression stack"
assert_file_contains "$WORK_DIR/direct_branch_compare_linux.s" 'cmpq \$1, %rax' "compiler x86_64 backend should compare simple branch immediates directly"
compile_and_check_native "$WORK_DIR/direct_branch_compare.c" "$WORK_DIR/direct_branch_compare_bin" 1 "compiler x86_64 backend should preserve direct branch comparison semantics"

cat > "$WORK_DIR/block_scratch_cache.c" <<'EOF'
int main(int argc, char **argv) {
    int left;
    int right;
    int extra;
    (void)argv;
    argc = argc + 1;
    argc = argc - 1;
    left = argc + 1;
    right = argc + 2;
    extra = argc + 3;
    return left + right + extra;
}
EOF

"$ROOT_DIR/build/ncc" -S --target linux-x86_64 "$WORK_DIR/block_scratch_cache.c" -o "$WORK_DIR/block_scratch_cache_linux.s"
assert_file_contains "$WORK_DIR/block_scratch_cache_linux.s" 'movq %rax, %r10' "compiler x86_64 backend should fill the intra-block scratch cache from stack locals"
assert_file_contains "$WORK_DIR/block_scratch_cache_linux.s" 'movslq %r10d, %rax' "compiler x86_64 backend should reuse the intra-block scratch cache"
compile_and_check_native "$WORK_DIR/block_scratch_cache.c" "$WORK_DIR/block_scratch_cache_bin" 9 "compiler x86_64 backend should preserve intra-block scratch cache semantics"

cat > "$WORK_DIR/block_branch_seed.c" <<'EOF'
int main(int argc, char **argv) {
    int left;
    int right;
    (void)argv;
    if (argc > 2) {
        left = argc + 3;
        right = argc + 4;
        argc = argc + 1;
        return left + right + argc;
    }
    return argc;
}
EOF

"$ROOT_DIR/build/ncc" -S --target linux-x86_64 "$WORK_DIR/block_branch_seed.c" -o "$WORK_DIR/block_branch_seed_linux.s"
assert_file_contains "$WORK_DIR/block_branch_seed_linux.s" 'cmpq \$2, %rax' "compiler x86_64 backend should directly compare branch operands before cache seeding"
assert_file_contains "$WORK_DIR/block_branch_seed_linux.s" 'movq %rax, %r10' "compiler x86_64 backend should seed the intra-block scratch cache from direct branch compares"
assert_file_contains "$WORK_DIR/block_branch_seed_linux.s" 'movslq %r10d, %rax' "compiler x86_64 backend should reuse branch-seeded scratch cache values"
compile_and_check_native "$WORK_DIR/block_branch_seed.c" "$WORK_DIR/block_branch_seed_bin" 1 "compiler x86_64 backend should preserve branch-seeded scratch cache semantics"

cat > "$WORK_DIR/mutated_local.c" <<'EOF'
int main(void) {
    int value = 1;
    value = value + 1;
    return value;
}
EOF

"$ROOT_DIR/build/ncc" -S --target linux-x86_64 "$WORK_DIR/mutated_local.c" -o "$WORK_DIR/mutated_local_linux.s"
if grep -q 'movq %rax, %rbx' "$WORK_DIR/mutated_local_linux.s"; then
    fail "compiler x86_64 backend should not cache mutated locals"
fi

cat > "$WORK_DIR/register_call_args.c" <<'EOF'
int pair(int left, int right) {
    return left + right;
}

int main(void) {
    return pair(1, 2);
}
EOF

"$ROOT_DIR/build/ncc" -S --target linux-x86_64 "$WORK_DIR/register_call_args.c" -o "$WORK_DIR/register_call_args_linux.s"
assert_file_contains "$WORK_DIR/register_call_args_linux.s" 'popq %rsi' "compiler x86_64 backend should pop register call args directly"
assert_file_contains "$WORK_DIR/register_call_args_linux.s" 'popq %rdi' "compiler x86_64 backend should pop register call args directly"

"$ROOT_DIR/build/ncc" -S --target macos-aarch64 "$WORK_DIR/sample.c" -o "$WORK_DIR/sample_macos.s"
assert_file_contains "$WORK_DIR/sample_macos.s" '^\.globl _main$' "compiler macOS AArch64 backend missing Darwin global symbol"
assert_file_contains "$WORK_DIR/sample_macos.s" 'movz x0, #42' "compiler macOS AArch64 backend missing immediate return code"
assert_command_succeeds "$ROOT_DIR/build/ncc" -Wno-pedantic -S --target macos-aarch64 "$WORK_DIR/sample.c" -o "$WORK_DIR/sample_warn_macos.s"
"$ROOT_DIR/build/ncc" -S --target macos-aarch64 -ffunction-sections -fdata-sections "$WORK_DIR/sample.c" -o "$WORK_DIR/sample_macos_sections.s"
assert_file_contains "$WORK_DIR/sample_macos_sections.s" '^\.subsections_via_symbols$' "compiler should enable Mach-O dead-strip subsections when requested"

if [ "$(uname -s)" = "Linux" ]; then
    "$ROOT_DIR/build/ncc" -c --target linux-x86_64 "$WORK_DIR/sample.c" -o "$WORK_DIR/sample_linux.o"
    "$ROOT_DIR/build/hexdump" "$WORK_DIR/sample_linux.o" > "$WORK_DIR/sample_linux_hex.out"
    assert_file_contains "$WORK_DIR/sample_linux_hex.out" '7f 45 4c 46' "compiler object writer did not emit ELF magic"

    "$ROOT_DIR/build/ncc" -S --target linux-x86_64 -ffunction-sections -fdata-sections "$WORK_DIR/sample.c" -o "$WORK_DIR/sample_linux_sections.s"
    assert_file_contains "$WORK_DIR/sample_linux_sections.s" '^\.section \.text\.main,"ax",@progbits$' "compiler should emit one text section per function when requested"
    assert_command_succeeds "$ROOT_DIR/build/ncc" --target linux-x86_64 -ffunction-sections -fdata-sections -Wl,--gc-sections "$WORK_DIR/sample.c" -o "$WORK_DIR/sample_gc_bin"

    cat > "$WORK_DIR/bss_globals.c" <<'EOF'
static char huge[1048576];
static long long counter;

int main(void) {
    huge[0] = 'o';
    huge[1] = 'k';
    counter = counter + huge[1];
    return counter == 'k' ? 0 : 1;
}
EOF

    "$ROOT_DIR/build/ncc" -S --target linux-x86_64 -ffunction-sections -fdata-sections "$WORK_DIR/bss_globals.c" -o "$WORK_DIR/bss_globals_linux.s"
    assert_file_contains "$WORK_DIR/bss_globals_linux.s" '^\.section \.bss\.huge,"aw",@nobits$' "compiler should place zero-initialized globals in dedicated BSS sections when requested"
    assert_file_contains "$WORK_DIR/bss_globals_linux.s" '^[[:space:]]*\.zero [1-9][0-9]*$' "compiler should emit zero-filled global storage without file-backed data bloat"
fi

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
assert_file_contains "$WORK_DIR/flow_linux.s" 'jge \.L' "compiler x86_64 backend should branch directly on relational conditions"

cat > "$WORK_DIR/bool_value.c" <<'EOF'
int main(int argc, char **argv) {
    (void)argv;
    return argc < 3;
}
EOF

"$ROOT_DIR/build/ncc" -S --target linux-x86_64 "$WORK_DIR/bool_value.c" -o "$WORK_DIR/bool_value_linux.s"
assert_file_contains "$WORK_DIR/bool_value_linux.s" 'setl %al' "compiler x86_64 backend should still materialize value-producing booleans"
assert_file_contains "$WORK_DIR/bool_value_linux.s" 'movzbl %al, %eax' "compiler x86_64 backend should compact boolean materialization"

cat > "$WORK_DIR/direct_branch.c" <<'EOF'
int probe(int value) {
    return value;
}

int main(int argc, char **argv) {
    (void)argv;
    if (probe(argc) != 0 && argc > 1) {
        return 0;
    }
    return 1;
}
EOF

"$ROOT_DIR/build/ncc" -S --target linux-x86_64 "$WORK_DIR/direct_branch.c" -o "$WORK_DIR/direct_branch_linux.s"
assert_file_contains "$WORK_DIR/direct_branch_linux.s" 'call probe' "compiler x86_64 backend should emit call in branch condition"
assert_file_contains "$WORK_DIR/direct_branch_linux.s" 'testq %rax, %rax' "compiler x86_64 backend should test call results directly in branches"
assert_file_contains "$WORK_DIR/direct_branch_linux.s" 'jle \.L' "compiler x86_64 backend should branch directly on && relational leaves"
if grep -q 'setne %al' "$WORK_DIR/direct_branch_linux.s"; then
    fail "compiler x86_64 backend should not materialize call != 0 branch conditions"
fi

"$ROOT_DIR/build/ncc" -S --target macos-aarch64 "$WORK_DIR/flow.c" -o "$WORK_DIR/flow_macos.s"
assert_file_contains "$WORK_DIR/flow_macos.s" 'b\.ge \.L[a-zA-Z0-9_]*_else[0-9][0-9]*' "compiler macOS AArch64 backend should branch directly on relational conditions"
assert_file_contains "$WORK_DIR/flow_macos.s" 'add x0, x1, x2' "compiler macOS AArch64 backend missing arithmetic lowering"

cat > "$WORK_DIR/pointer_scale.c" <<'EOF'
long left_index(long *p) {
    return *(p + 2);
}

long right_index(long *p) {
    return *(2 + p);
}
EOF

"$ROOT_DIR/build/ncc" -S --target linux-x86_64 "$WORK_DIR/pointer_scale.c" -o "$WORK_DIR/pointer_scale_linux.s"
assert_file_contains "$WORK_DIR/pointer_scale_linux.s" 'salq \$3, %rax' "compiler x86_64 backend should scale pointer offsets with a shift"
assert_file_contains "$WORK_DIR/pointer_scale_linux.s" 'salq \$3, %rcx' "compiler x86_64 backend should scale stacked pointer offsets with a shift"

"$ROOT_DIR/build/ncc" -S --target macos-aarch64 "$WORK_DIR/pointer_scale.c" -o "$WORK_DIR/pointer_scale_macos.s"
assert_file_contains "$WORK_DIR/pointer_scale_macos.s" 'lsl x0, x0, #3' "compiler AArch64 backend should scale pointer offsets with a shift"
assert_file_contains "$WORK_DIR/pointer_scale_macos.s" 'lsl x9, x9, #3' "compiler AArch64 backend should scale stacked pointer offsets with a shift"

cat > "$WORK_DIR/same_value_compare.c" <<'EOF'
int main(void) {
    int value = 7;
    if (value != value) {
        return 1;
    }
    if (value <= value) {
        return 0;
    }
    return 2;
}
EOF

"$ROOT_DIR/build/ncc" --dump-ir --target linux-x86_64 "$WORK_DIR/same_value_compare.c" > "$WORK_DIR/same_value_compare_linux.ir"
assert_file_contains "$WORK_DIR/same_value_compare_linux.ir" '^ret 0$' "compiler IR should fold same-value integer comparisons on Linux target"
"$ROOT_DIR/build/ncc" --dump-ir --target macos-aarch64 "$WORK_DIR/same_value_compare.c" > "$WORK_DIR/same_value_compare_macos.ir"
assert_file_contains "$WORK_DIR/same_value_compare_macos.ir" '^ret 0$' "compiler IR should fold same-value integer comparisons on macOS target"

cat > "$WORK_DIR/same_arm_conditional.c" <<'EOF'
int main(int argc, char **argv) {
    (void)argv;
    return argc ? 7 : 7;
}
EOF

"$ROOT_DIR/build/ncc" --dump-ir --target linux-x86_64 "$WORK_DIR/same_arm_conditional.c" > "$WORK_DIR/same_arm_conditional_linux.ir"
assert_file_contains "$WORK_DIR/same_arm_conditional_linux.ir" '^ret 7$' "compiler IR should fold same-arm conditionals on Linux target"
"$ROOT_DIR/build/ncc" --dump-ir --target macos-aarch64 "$WORK_DIR/same_arm_conditional.c" > "$WORK_DIR/same_arm_conditional_macos.ir"
assert_file_contains "$WORK_DIR/same_arm_conditional_macos.ir" '^ret 7$' "compiler IR should fold same-arm conditionals on macOS target"

cat > "$WORK_DIR/side_effect_conditional.c" <<'EOF'
static int bump(int *slot) {
    *slot += 1;
    return *slot;
}

int main(void) {
    int value = 0;
    return bump(&value) ? 7 : 7;
}
EOF

"$ROOT_DIR/build/ncc" --dump-ir --target macos-aarch64 "$WORK_DIR/side_effect_conditional.c" > "$WORK_DIR/side_effect_conditional.ir"
assert_file_contains "$WORK_DIR/side_effect_conditional.ir" 'bump(&value) ? 7 : 7' "compiler IR must preserve side-effecting same-arm conditional predicates"

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

cat > "$WORK_DIR/termios_mask.c" <<'EOF'
#include <termios.h>

int update_flags(struct termios *raw) {
    raw->c_oflag &= ~(tcflag_t)(OPOST);
    return 0;
}
EOF

assert_command_succeeds "$ROOT_DIR/build/ncc" -S --target macos-aarch64 "$WORK_DIR/termios_mask.c" -o "$WORK_DIR/termios_mask_macos.s"

cat > "$WORK_DIR/prefix_member_incdec.c" <<'EOF'
typedef struct {
    int count;
    int items[4];
} Stack;

int main(void) {
    Stack stack;
    stack.count = 2;
    stack.items[0] = 11;
    stack.items[1] = 22;
    return stack.items[--stack.count] == 22 ? 0 : 1;
}
EOF

assert_command_succeeds "$ROOT_DIR/build/ncc" -S --target macos-aarch64 "$WORK_DIR/prefix_member_incdec.c" -o "$WORK_DIR/prefix_member_incdec_macos.s"

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

if [ "$(uname -s)" = "Linux" ]; then
    assert_command_succeeds "$ROOT_DIR/build/ncc" -c --target linux-x86_64 "$ROOT_DIR/src/shared/runtime/unicode.c" -o "$WORK_DIR/unicode_repo.o"
    "$ROOT_DIR/build/hexdump" "$WORK_DIR/unicode_repo.o" > "$WORK_DIR/unicode_repo_hex.out"
    assert_file_contains "$WORK_DIR/unicode_repo_hex.out" '7f 45 4c 46' "compiler failed on a large repository-scale Unicode initializer"
fi
