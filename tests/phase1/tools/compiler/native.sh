#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

phase1_compiler_setup native

cat > "$WORK_DIR/sample.c" <<'EOF'
int main(void) {
    return 42;
}
EOF

compile_and_check_native "$WORK_DIR/sample.c" "$WORK_DIR/sample_native_bin" "42" "compiler linker did not produce a runnable executable"

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

compile_and_check_native "$WORK_DIR/flow.c" "$WORK_DIR/flow_native_bin" "4" "compiler linker did not preserve control-flow semantics"

cat > "$WORK_DIR/backend_expr.c" <<'EOF'
int main(void) {
    char buffer[16];
    buffer[0] = "ok"[0];
    buffer[1] = "ok"[1];
    buffer[2] = '\0';
    return buffer[1] == 'k' ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/backend_expr.c" "$WORK_DIR/backend_expr_bin" "0" "compiler backend did not preserve string/index expression semantics"

cat > "$WORK_DIR/long_expr.c" <<'EOF'
int main(void) {
    return 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1;
}
EOF

compile_and_check_native "$WORK_DIR/long_expr.c" "$WORK_DIR/long_expr_bin" "64" "compiler failed on a repository-scale long expression"

cat > "$WORK_DIR/static_local_string_array.c" <<'EOF'
int main(void) {
    static const char punctuation[] = "{}[]()#";
    return punctuation[2] == '[' ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/static_local_string_array.c" "$WORK_DIR/static_local_string_array_bin" "0" "compiler failed on a function-local static string array initializer"

cat > "$WORK_DIR/local_struct_init.c" <<'EOF'
typedef struct {
    int first;
    int second;
} Pair;

int main(void) {
    Pair pair = { 3, 4 };
    return pair.first == 3 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/local_struct_init.c" "$WORK_DIR/local_struct_init_bin" "0" "compiler failed on a function-local aggregate initializer"

cat > "$WORK_DIR/escaped_char_literal.c" <<'EOF'
int main(void) {
    char quote = '\'';
    return quote == '\'' ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/escaped_char_literal.c" "$WORK_DIR/escaped_char_literal_bin" "0" "compiler failed on an escaped single-quote character literal"

cat > "$WORK_DIR/adjacent_strings.c" <<'EOF'
int main(void) {
    const char *text = "hello, " "world";
    return text[7] == 'w' ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/adjacent_strings.c" "$WORK_DIR/adjacent_strings_bin" "0" "compiler failed on adjacent string literal concatenation"

cat > "$WORK_DIR/comment_macro.c" <<'EOF'
#define VALUE 0 /* inline expansion payload */
/*
 * VALUE should remain ordinary comment text here.
 */
int main(void) {
    return VALUE;
}
EOF

compile_and_check_native "$WORK_DIR/comment_macro.c" "$WORK_DIR/comment_macro_bin" "0" "preprocessor expanded a macro inside a block comment"

cat > "$WORK_DIR/multi_arg_call.c" <<'EOF'
int check_args(int number, const char *text) {
    return number == 7 && text[0] == 'o' ? 0 : 1;
}

int main(void) {
    return check_args(7, "ok");
}
EOF

compile_and_check_native "$WORK_DIR/multi_arg_call.c" "$WORK_DIR/multi_arg_call_bin" "0" "compiler failed to pass multiple call arguments correctly"

cat > "$WORK_DIR/multi_file_helper.c" <<'EOF'
int helper_value(void) {
    return 41;
}
EOF

cat > "$WORK_DIR/multi_file_main.c" <<'EOF'
int helper_value(void);

int main(void) {
    return helper_value() == 41 ? 0 : 1;
}
EOF

if [ -n "$RUN_TARGET" ]; then
    assert_command_succeeds "$ROOT_DIR/build/ncc" --target "$RUN_TARGET" "$WORK_DIR/multi_file_main.c" "$WORK_DIR/multi_file_helper.c" -o "$WORK_DIR/multi_file_bin"
    if "$WORK_DIR/multi_file_bin"; then
        actual_status=0
    else
        actual_status=$?
    fi
    assert_text_equals "$actual_status" "0" "compiler multi-file linker flow did not produce a runnable executable"
fi

cat > "$WORK_DIR/many_arg_call.c" <<'EOF'
int check_many(int a, int b, int c, int d, int e, int f, int g, int h, int i) {
    return a == 1 && b == 2 && c == 3 && d == 4 && e == 5 && f == 6 && g == 7 && h == 8 && i == 9 ? 0 : 1;
}

int main(void) {
    return check_many(1, 2, 3, 4, 5, 6, 7, 8, 9);
}
EOF

compile_and_check_native "$WORK_DIR/many_arg_call.c" "$WORK_DIR/many_arg_call_bin" "0" "compiler failed to preserve arguments beyond the register-only calling convention"

cat > "$WORK_DIR/branch_separator_string.c" <<'EOF'
int check_text(int code, const char *text) {
    return code == 1 && text[1] == '-' && text[2] == '>' ? 0 : 1;
}

int main(void) {
    if (check_text(1, " ->") != 0) {
        return 1;
    }
    return 0;
}
EOF

compile_and_check_native "$WORK_DIR/branch_separator_string.c" "$WORK_DIR/branch_separator_string_bin" "0" "compiler confused a quoted ' ->' string with an IR branch separator"

cat > "$WORK_DIR/casted_member_lvalue.c" <<'EOF'
typedef struct {
    unsigned char code;
} Box;

int main(void) {
    static unsigned char storage[16];
    ((Box *)storage)->code = 7;
    return ((Box *)storage)->code == 7 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/casted_member_lvalue.c" "$WORK_DIR/casted_member_lvalue_bin" "0" "compiler failed on a casted pointer member assignment lvalue"

cat > "$WORK_DIR/shadowed_local_name.c" <<'EOF'
static int path_is_nonempty(char **argv) {
    return argv[0][0] != '\0';
}

int main(int argc, char **argv) {
    int guard = 0;
    if (argc > 100) {
        int j = 1;
        guard = j;
    }
    if (guard == 0) {
        int j = 0;
        if (path_is_nonempty(argv)) {
            return j;
        }
        return 2;
    }
    return 1;
}
EOF

compile_and_check_native "$WORK_DIR/shadowed_local_name.c" "$WORK_DIR/shadowed_local_name_bin" "0" "compiler corrupted a shadowed block-local with the same name in a later scope"

cat > "$WORK_DIR/implicit_fallthrough_return.c" <<'EOF'
static void copy_text(char *dst, int limit, const char *src) {
    int i = 0;
    if (limit == 0) {
        return;
    }
    while (src[i] != '\0' && i + 1 < limit) {
        dst[i] = src[i];
        i = i + 1;
    }
    dst[i] = '\0';
}

int main(int argc, char **argv) {
    char buf[32];
    copy_text(buf, 32, argc > 0 ? argv[0] : "copy");
    return buf[0] != '\0' ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/implicit_fallthrough_return.c" "$WORK_DIR/implicit_fallthrough_return_bin" "0" "compiler omitted the function epilogue after an early return in a void helper"

cat > "$WORK_DIR/int128_cast.c" <<'EOF'
int main(void) {
    unsigned __int128 root = 42;
    long long value = (__int128)root;
    return value == 42 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/int128_cast.c" "$WORK_DIR/int128_cast_bin" "0" "compiler failed on __int128 cast expressions"

"$ROOT_DIR/build/ncc" -c "$WORK_DIR/sample.c" -o "$WORK_DIR/default_host.o"
"$ROOT_DIR/build/hexdump" "$WORK_DIR/default_host.o" > "$WORK_DIR/default_host_hex.out"
if ! grep -q '7f 45 4c 46' "$WORK_DIR/default_host_hex.out" && ! grep -q 'cf fa ed fe' "$WORK_DIR/default_host_hex.out"; then
    fail "compiler default target did not emit a supported object format"
fi
"$ROOT_DIR/build/ncc" "$WORK_DIR/sample.c" -o "$WORK_DIR/default_host_bin"
if "$WORK_DIR/default_host_bin"; then
    default_link_status=0
else
    default_link_status=$?
fi
assert_text_equals "$default_link_status" "42" "compiler default target did not link a runnable executable"
