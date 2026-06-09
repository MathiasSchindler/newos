#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"
. "$ROOT_DIR/tests/lib/build.sh"

newos_configure_test_tools

LINKER=${NEWOS_LINKER:-$TEST_BIN_DIR/linker}
WORK_DIR="$ROOT_DIR/tests/tmp/linker_cli"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

note "linker cli"

if [ ! -x "$LINKER" ]; then
    fail "missing linker: $LINKER"
fi

linker_macho_status=0
"$LINKER" --target=mach-o-arm64 -o "$WORK_DIR/linker_macho.out" "$WORK_DIR/missing-a.o" > "$WORK_DIR/linker_macho.stdout" 2> "$WORK_DIR/linker_macho.stderr" || linker_macho_status=$?
assert_text_equals "$linker_macho_status" '1' "linker Mach-O target should reject missing inputs"
assert_file_contains "$WORK_DIR/linker_macho.stderr" 'failed to open object' "linker Mach-O target did not report the missing input"

if [ "$(uname -s 2>/dev/null || echo unknown)" = Darwin ] && command -v clang >/dev/null 2>&1; then
    cat > "$WORK_DIR/macho_start.s" <<'ASM'
.globl _start
.p2align 2
_start:
    mov x0, #0
    mov x16, #1
    svc #0x80
ASM
    if clang -target arm64-apple-macos11 -c "$WORK_DIR/macho_start.s" -o "$WORK_DIR/macho_start.o" > "$WORK_DIR/macho_start_compile.out" 2>&1; then
        "$LINKER" --target=mach-o-arm64 --map "$WORK_DIR/macho_start.map" -o "$WORK_DIR/macho_start" "$WORK_DIR/macho_start.o" > "$WORK_DIR/linker_macho_start.stdout" 2> "$WORK_DIR/linker_macho_start.stderr"
        assert_file_contains "$WORK_DIR/macho_start.map" 'newos macho linker map' "Mach-O backend did not write a map file"
        assert_file_contains "$WORK_DIR/macho_start.map" 'Final sections' "Mach-O map did not report final sections"
        assert_file_contains "$WORK_DIR/macho_start.map" 'input-section' "Mach-O map did not report input sections"
        assert_file_contains "$WORK_DIR/macho_start.map" 'symbol .* _start ' "Mach-O map did not report input symbols"
        od -An -tx1 -N4 "$WORK_DIR/macho_start" | tr -d ' \n' > "$WORK_DIR/macho_start.magic"
        assert_file_contains "$WORK_DIR/macho_start.magic" '^cffaedfe$' "Mach-O backend did not write a 64-bit Mach-O executable"
        if command -v otool >/dev/null 2>&1; then
            otool -l "$WORK_DIR/macho_start" > "$WORK_DIR/macho_start.otool"
            assert_file_contains "$WORK_DIR/macho_start.otool" 'LC_MAIN' "Mach-O backend did not write LC_MAIN"
            assert_file_contains "$WORK_DIR/macho_start.otool" 'LC_DYLD_INFO_ONLY' "Mach-O backend did not write dyld rebase metadata command"
            assert_file_contains "$WORK_DIR/macho_start.otool" 'rebase_size 0' "Mach-O backend should report empty rebase metadata for relocation-free output"
            assert_file_contains "$WORK_DIR/macho_start.otool" 'LC_CODE_SIGNATURE' "Mach-O backend did not write LC_CODE_SIGNATURE"
        fi
        if command -v codesign >/dev/null 2>&1; then
            codesign --verify --strict "$WORK_DIR/macho_start" > "$WORK_DIR/macho_start_codesign.out" 2>&1
        fi
        if [ "$(uname -m 2>/dev/null || echo unknown)" = arm64 ]; then
            "$WORK_DIR/macho_start"
        fi
    else
        echo "skipping Mach-O executable test; clang could not emit arm64 Mach-O assembly"
    fi

    cat > "$WORK_DIR/macho_external.s" <<'ASM'
.globl _start
.p2align 2
_start:
    bl _missing_symbol
    mov x0, #0
    mov x16, #1
    svc #0x80
ASM
    if clang -target arm64-apple-macos11 -c "$WORK_DIR/macho_external.s" -o "$WORK_DIR/macho_external.o" > "$WORK_DIR/macho_external_compile.out" 2>&1; then
        linker_macho_external_status=0
        "$LINKER" --target=mach-o-arm64 -o "$WORK_DIR/macho_external" "$WORK_DIR/macho_external.o" > "$WORK_DIR/linker_macho_external.stdout" 2> "$WORK_DIR/linker_macho_external.stderr" || linker_macho_external_status=$?
        assert_text_equals "$linker_macho_external_status" '1' "Mach-O backend should reject undefined branch targets"
        assert_file_contains "$WORK_DIR/linker_macho_external.stderr" 'undefined Mach-O arm64 symbol' "Mach-O backend did not diagnose undefined branch targets"
    fi

    cat > "$WORK_DIR/macho_call_local.s" <<'ASM'
.globl _start
.p2align 2
_start:
    bl helper
    mov x16, #1
    svc #0x80
helper:
    mov x0, #0
    ret
ASM
    if clang -target arm64-apple-macos11 -c "$WORK_DIR/macho_call_local.s" -o "$WORK_DIR/macho_call_local.o" > "$WORK_DIR/macho_call_local_compile.out" 2>&1; then
        "$LINKER" --target=mach-o-arm64 -o "$WORK_DIR/macho_call_local" "$WORK_DIR/macho_call_local.o" > "$WORK_DIR/linker_macho_call_local.stdout" 2> "$WORK_DIR/linker_macho_call_local.stderr"
        if command -v codesign >/dev/null 2>&1; then
            codesign --verify --strict "$WORK_DIR/macho_call_local" > "$WORK_DIR/macho_call_local_codesign.out" 2>&1
        fi
        if [ "$(uname -m 2>/dev/null || echo unknown)" = arm64 ]; then
            "$WORK_DIR/macho_call_local"
        fi
    fi

    cat > "$WORK_DIR/macho_data_ref.s" <<'ASM'
.data
.p2align 3
value:
    .quad 7
.text
.globl _start
.p2align 2
_start:
    adrp x1, value@PAGE
    add x1, x1, value@PAGEOFF
    mov x0, #0
    mov x16, #1
    svc #0x80
ASM
    if clang -target arm64-apple-macos11 -c "$WORK_DIR/macho_data_ref.s" -o "$WORK_DIR/macho_data_ref.o" > "$WORK_DIR/macho_data_ref_compile.out" 2>&1; then
        "$LINKER" --target=mach-o-arm64 -o "$WORK_DIR/macho_data_ref" "$WORK_DIR/macho_data_ref.o" > "$WORK_DIR/linker_macho_data_ref.stdout" 2> "$WORK_DIR/linker_macho_data_ref.stderr"
        if command -v otool >/dev/null 2>&1; then
            otool -l "$WORK_DIR/macho_data_ref" > "$WORK_DIR/macho_data_ref.otool"
            assert_file_contains "$WORK_DIR/macho_data_ref.otool" '__DATA' "Mach-O backend did not write a __DATA segment for data references"
        fi
        if command -v codesign >/dev/null 2>&1; then
            codesign --verify --strict "$WORK_DIR/macho_data_ref" > "$WORK_DIR/macho_data_ref_codesign.out" 2>&1
        fi
    fi

    cat > "$WORK_DIR/macho_cstring_ref.s" <<'ASM'
.cstring
Lstr:
    .asciz "hello"
.text
.globl _start
.p2align 2
_start:
    adrp x1, Lstr@PAGE
    add x1, x1, Lstr@PAGEOFF
    mov x0, #0
    mov x16, #1
    svc #0x80
ASM
    if clang -target arm64-apple-macos11 -c "$WORK_DIR/macho_cstring_ref.s" -o "$WORK_DIR/macho_cstring_ref.o" > "$WORK_DIR/macho_cstring_ref_compile.out" 2>&1; then
        "$LINKER" --target=mach-o-arm64 -o "$WORK_DIR/macho_cstring_ref" "$WORK_DIR/macho_cstring_ref.o" > "$WORK_DIR/linker_macho_cstring_ref.stdout" 2> "$WORK_DIR/linker_macho_cstring_ref.stderr"
        if command -v otool >/dev/null 2>&1; then
            otool -l "$WORK_DIR/macho_cstring_ref" > "$WORK_DIR/macho_cstring_ref.otool"
            assert_file_contains "$WORK_DIR/macho_cstring_ref.otool" '__cstring' "Mach-O backend did not preserve __TEXT,__cstring"
        fi
        if command -v codesign >/dev/null 2>&1; then
            codesign --verify --strict "$WORK_DIR/macho_cstring_ref" > "$WORK_DIR/macho_cstring_ref_codesign.out" 2>&1
        fi
    fi

    cat > "$WORK_DIR/macho_addend_ref.s" <<'ASM'
.cstring
Lstr:
    .asciz "0123456789"
.text
.globl _start
.p2align 2
_start:
    adrp x1, Lstr@PAGE+4
    add x1, x1, Lstr@PAGEOFF+4
    ldrb w0, [x1]
    sub w0, w0, #'4'
    mov x16, #1
    svc #0x80
ASM
    if clang -target arm64-apple-macos11 -c "$WORK_DIR/macho_addend_ref.s" -o "$WORK_DIR/macho_addend_ref.o" > "$WORK_DIR/macho_addend_ref_compile.out" 2>&1; then
        "$LINKER" --target=mach-o-arm64 -o "$WORK_DIR/macho_addend_ref" "$WORK_DIR/macho_addend_ref.o" > "$WORK_DIR/linker_macho_addend_ref.stdout" 2> "$WORK_DIR/linker_macho_addend_ref.stderr"
        if command -v codesign >/dev/null 2>&1; then
            codesign --verify --strict "$WORK_DIR/macho_addend_ref" > "$WORK_DIR/macho_addend_ref_codesign.out" 2>&1
        fi
        if [ "$(uname -m 2>/dev/null || echo unknown)" = arm64 ]; then
            "$WORK_DIR/macho_addend_ref"
        fi
    fi

    cat > "$WORK_DIR/macho_clang_start.c" <<'C'
__attribute__((noreturn)) static void sys_exit(long code) {
    register long x0 __asm__("x0") = code;
    register long x16 __asm__("x16") = 1;
    __asm__ volatile("svc #0x80" : : "r"(x0), "r"(x16) : "memory");
    for (;;) {}
}
static long sys_write(long fd, const void *buf, unsigned long len) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = (long)len;
    register long x16 __asm__("x16") = 4;
    __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x16) : "memory");
    return x0;
}
__attribute__((noreturn)) void _start(void) {
    static const char msg[] = "tiny clang ok\n";
    (void)sys_write(1, msg, sizeof(msg) - 1);
    sys_exit(0);
}
C
    if clang -target arm64-apple-macos11 -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-exceptions \
        -c "$WORK_DIR/macho_clang_start.c" -o "$WORK_DIR/macho_clang_start.o" > "$WORK_DIR/macho_clang_start_compile.out" 2>&1; then
        "$LINKER" --target=mach-o-arm64 -o "$WORK_DIR/macho_clang_start" "$WORK_DIR/macho_clang_start.o" > "$WORK_DIR/linker_macho_clang_start.stdout" 2> "$WORK_DIR/linker_macho_clang_start.stderr"
        if command -v otool >/dev/null 2>&1; then
            otool -l "$WORK_DIR/macho_clang_start" > "$WORK_DIR/macho_clang_start.otool"
            assert_file_contains "$WORK_DIR/macho_clang_start.otool" '__const' "Mach-O backend did not preserve clang __TEXT,__const"
        fi
        if command -v codesign >/dev/null 2>&1; then
            codesign --verify --strict "$WORK_DIR/macho_clang_start" > "$WORK_DIR/macho_clang_start_codesign.out" 2>&1
        fi
        if [ "$(uname -m 2>/dev/null || echo unknown)" = arm64 ]; then
            "$WORK_DIR/macho_clang_start" > "$WORK_DIR/macho_clang_start.run"
            assert_file_contains "$WORK_DIR/macho_clang_start.run" 'tiny clang ok' "Mach-O clang C executable did not run correctly"
        fi
    fi

    cat > "$WORK_DIR/macho_rebase_pointer.c" <<'C'
static const char message[] = "rebase ok\n";
static const char *message_pointer = message;
static long sys_write(long fd, const void *buf, unsigned long len) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = (long)len;
    register long x16 __asm__("x16") = 4;
    __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x16) : "memory");
    return x0;
}
__attribute__((noreturn)) static void sys_exit(long code) {
    register long x0 __asm__("x0") = code;
    register long x16 __asm__("x16") = 1;
    __asm__ volatile("svc #0x80" : : "r"(x0), "r"(x16) : "memory");
    for (;;) {}
}
void _start(void) {
    (void)sys_write(1, message_pointer, 10);
    sys_exit(0);
}
C
    if clang -target arm64-apple-macos11 -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-exceptions \
        -c "$WORK_DIR/macho_rebase_pointer.c" -o "$WORK_DIR/macho_rebase_pointer.o" > "$WORK_DIR/macho_rebase_pointer_compile.out" 2>&1; then
        "$LINKER" --target=mach-o-arm64 --macho-compact --gc-sections -o "$WORK_DIR/macho_rebase_pointer" "$WORK_DIR/macho_rebase_pointer.o" > "$WORK_DIR/linker_macho_rebase_pointer.stdout" 2> "$WORK_DIR/linker_macho_rebase_pointer.stderr"
        if command -v otool >/dev/null 2>&1; then
            otool -l "$WORK_DIR/macho_rebase_pointer" > "$WORK_DIR/macho_rebase_pointer.otool"
            assert_file_contains "$WORK_DIR/macho_rebase_pointer.otool" 'LC_DYLD_INFO_ONLY' "Mach-O pointer-rebase output did not write dyld info"
            assert_file_contains "$WORK_DIR/macho_rebase_pointer.otool" 'rebase_size [1-9]' "Mach-O pointer-rebase output did not report rebase opcodes"
        fi
        if command -v codesign >/dev/null 2>&1; then
            codesign --verify --strict "$WORK_DIR/macho_rebase_pointer" > "$WORK_DIR/macho_rebase_pointer_codesign.out" 2>&1
        fi
        if [ "$(uname -m 2>/dev/null || echo unknown)" = arm64 ]; then
            "$WORK_DIR/macho_rebase_pointer" > "$WORK_DIR/macho_rebase_pointer.run"
            assert_file_contains "$WORK_DIR/macho_rebase_pointer.run" 'rebase ok' "Mach-O pointer-rebase executable did not run correctly"
        fi
    fi

    if clang -target arm64-apple-macos11 -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-exceptions -flto \
        -c "$WORK_DIR/macho_clang_start.c" -o "$WORK_DIR/macho_clang_start_lto.o" > "$WORK_DIR/macho_clang_start_lto_compile.out" 2>&1; then
        "$LINKER" --target=mach-o-arm64 --lto-cc=clang -o "$WORK_DIR/macho_clang_start_lto" "$WORK_DIR/macho_clang_start_lto.o" > "$WORK_DIR/linker_macho_clang_start_lto.stdout" 2> "$WORK_DIR/linker_macho_clang_start_lto.stderr"
        od -An -tx1 -N4 "$WORK_DIR/macho_clang_start_lto" | tr -d ' \n' > "$WORK_DIR/macho_clang_start_lto.magic"
        assert_file_contains "$WORK_DIR/macho_clang_start_lto.magic" '^cffaedfe$' "Mach-O clang LTO path did not write an executable"
        if command -v codesign >/dev/null 2>&1; then
            codesign --verify --strict "$WORK_DIR/macho_clang_start_lto" > "$WORK_DIR/macho_clang_start_lto_codesign.out" 2>&1
        fi
        if [ "$(uname -m 2>/dev/null || echo unknown)" = arm64 ]; then
            "$WORK_DIR/macho_clang_start_lto" > "$WORK_DIR/macho_clang_start_lto.run"
            assert_file_contains "$WORK_DIR/macho_clang_start_lto.run" 'tiny clang ok' "Mach-O clang LTO executable did not run correctly"
        fi
    fi

    cat > "$WORK_DIR/macho_split_start.c" <<'C'
extern const char *helper_message(void);
__attribute__((noreturn)) static void sys_exit(long code) {
    register long x0 __asm__("x0") = code;
    register long x16 __asm__("x16") = 1;
    __asm__ volatile("svc #0x80" : : "r"(x0), "r"(x16) : "memory");
    for (;;) {}
}
static long sys_write(long fd, const void *buf, unsigned long len) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = (long)len;
    register long x16 __asm__("x16") = 4;
    __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x16) : "memory");
    return x0;
}
static unsigned long c_strlen(const char *text) {
    unsigned long len = 0;
    while (text[len] != '\0') {
        len += 1;
    }
    return len;
}
__attribute__((noreturn)) void _start(void) {
    const char *msg = helper_message();
    (void)sys_write(1, msg, c_strlen(msg));
    sys_exit(0);
}
C
    cat > "$WORK_DIR/macho_split_helper.c" <<'C'
const char *helper_message(void) {
    static int counter;
    static const char first[] = "multi object ok\n";
    static const char later[] = "bad\n";
    counter += 1;
    return counter == 1 ? first : later;
}
C
    if clang -target arm64-apple-macos11 -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-exceptions \
        -c "$WORK_DIR/macho_split_start.c" -o "$WORK_DIR/macho_split_start.o" > "$WORK_DIR/macho_split_start_compile.out" 2>&1 && \
       clang -target arm64-apple-macos11 -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-exceptions \
        -c "$WORK_DIR/macho_split_helper.c" -o "$WORK_DIR/macho_split_helper.o" > "$WORK_DIR/macho_split_helper_compile.out" 2>&1; then
        "$LINKER" --target=mach-o-arm64 -o "$WORK_DIR/macho_split" "$WORK_DIR/macho_split_start.o" "$WORK_DIR/macho_split_helper.o" > "$WORK_DIR/linker_macho_split.stdout" 2> "$WORK_DIR/linker_macho_split.stderr"
        if command -v otool >/dev/null 2>&1; then
            otool -l "$WORK_DIR/macho_split" > "$WORK_DIR/macho_split.otool"
            assert_file_contains "$WORK_DIR/macho_split.otool" '__bss' "Mach-O backend did not preserve clang __DATA,__bss"
        fi
        if command -v codesign >/dev/null 2>&1; then
            codesign --verify --strict "$WORK_DIR/macho_split" > "$WORK_DIR/macho_split_codesign.out" 2>&1
        fi
        if [ "$(uname -m 2>/dev/null || echo unknown)" = arm64 ]; then
            "$WORK_DIR/macho_split" > "$WORK_DIR/macho_split.run"
            assert_file_contains "$WORK_DIR/macho_split.run" 'multi object ok' "Mach-O multi-object clang executable did not run correctly"
        fi
        if command -v ar >/dev/null 2>&1; then
            (cd "$WORK_DIR" && ar rcs libmacho_split_helper.a macho_split_helper.o) > "$WORK_DIR/macho_split_archive.out" 2>&1
            "$LINKER" --target=mach-o-arm64 -o "$WORK_DIR/macho_split_archive" "$WORK_DIR/macho_split_start.o" "$WORK_DIR/libmacho_split_helper.a" > "$WORK_DIR/linker_macho_split_archive.stdout" 2> "$WORK_DIR/linker_macho_split_archive.stderr"
            if command -v codesign >/dev/null 2>&1; then
                codesign --verify --strict "$WORK_DIR/macho_split_archive" > "$WORK_DIR/macho_split_archive_codesign.out" 2>&1
            fi
            if [ "$(uname -m 2>/dev/null || echo unknown)" = arm64 ]; then
                "$WORK_DIR/macho_split_archive" > "$WORK_DIR/macho_split_archive.run"
                assert_file_contains "$WORK_DIR/macho_split_archive.run" 'multi object ok' "Mach-O archive member executable did not run correctly"
            fi
        fi
    fi

    if clang -target arm64-apple-macos11 -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-exceptions -flto \
        -c "$WORK_DIR/macho_split_start.c" -o "$WORK_DIR/macho_split_start_lto.o" > "$WORK_DIR/macho_split_start_lto_compile.out" 2>&1 && \
       clang -target arm64-apple-macos11 -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-exceptions -flto \
        -c "$WORK_DIR/macho_split_helper.c" -o "$WORK_DIR/macho_split_helper_lto.o" > "$WORK_DIR/macho_split_helper_lto_compile.out" 2>&1; then
        "$LINKER" --target=mach-o-arm64 --lto-cc=clang -o "$WORK_DIR/macho_split_lto" "$WORK_DIR/macho_split_start_lto.o" "$WORK_DIR/macho_split_helper_lto.o" > "$WORK_DIR/linker_macho_split_lto.stdout" 2> "$WORK_DIR/linker_macho_split_lto.stderr"
        if command -v codesign >/dev/null 2>&1; then
            codesign --verify --strict "$WORK_DIR/macho_split_lto" > "$WORK_DIR/macho_split_lto_codesign.out" 2>&1
        fi
        if [ "$(uname -m 2>/dev/null || echo unknown)" = arm64 ]; then
            "$WORK_DIR/macho_split_lto" > "$WORK_DIR/macho_split_lto.run"
            assert_file_contains "$WORK_DIR/macho_split_lto.run" 'multi object ok' "Mach-O multi-input clang LTO executable did not run correctly"
        fi

        "$LINKER" --target=mach-o-arm64 --macho-compact --gc-sections --stats --lto-cc=clang -o "$WORK_DIR/macho_split_lto_compact" "$WORK_DIR/macho_split_start_lto.o" "$WORK_DIR/macho_split_helper_lto.o" > "$WORK_DIR/linker_macho_split_lto_compact.stdout" 2> "$WORK_DIR/linker_macho_split_lto_compact.stderr"
        assert_file_contains "$WORK_DIR/linker_macho_split_lto_compact.stdout" 'Mach-O linker stats' "Mach-O --stats did not print a stats header"
        assert_file_contains "$WORK_DIR/linker_macho_split_lto_compact.stdout" 'policy: compact' "Mach-O --macho-compact did not report compact policy"
        "$LINKER" --target=mach-o-arm64 --macho-compact --page-align --stats --lto-cc=clang -o "$WORK_DIR/macho_split_lto_page_align" "$WORK_DIR/macho_split_start_lto.o" "$WORK_DIR/macho_split_helper_lto.o" > "$WORK_DIR/linker_macho_split_lto_page_align.stdout" 2> "$WORK_DIR/linker_macho_split_lto_page_align.stderr"
        assert_file_contains "$WORK_DIR/linker_macho_split_lto_page_align.stdout" 'policy: page-aligned' "Mach-O --page-align did not disable compact policy"
        if command -v otool >/dev/null 2>&1; then
            otool -l "$WORK_DIR/macho_split_lto_compact" > "$WORK_DIR/macho_split_lto_compact.otool"
            assert_file_contains "$WORK_DIR/macho_split_lto_compact.otool" 'LC_CODE_SIGNATURE' "Mach-O compact output did not keep code signature load command"
            assert_file_contains "$WORK_DIR/macho_split_lto_compact.otool" 'ntools 0' "Mach-O compact output did not omit the LC_BUILD_VERSION tool record"
        fi
        if command -v codesign >/dev/null 2>&1; then
            codesign --verify --strict "$WORK_DIR/macho_split_lto_compact" > "$WORK_DIR/macho_split_lto_compact_codesign.out" 2>&1
        fi
        if [ "$(uname -m 2>/dev/null || echo unknown)" = arm64 ]; then
            "$WORK_DIR/macho_split_lto_compact" > "$WORK_DIR/macho_split_lto_compact.run"
            assert_file_contains "$WORK_DIR/macho_split_lto_compact.run" 'multi object ok' "Mach-O compact LTO executable did not run correctly"
        fi
    fi

    cat > "$WORK_DIR/macho_arm64.c" <<'C'
int linker_macho_probe(void) { return 42; }
C
    if clang -target arm64-apple-macos11 -c "$WORK_DIR/macho_arm64.c" -o "$WORK_DIR/macho_arm64.o" > "$WORK_DIR/macho_arm64_compile.out" 2>&1; then
        linker_macho_object_status=0
        "$LINKER" -m x86_64-linux -o "$WORK_DIR/macho_arm64.bin" "$WORK_DIR/macho_arm64.o" > "$WORK_DIR/linker_macho_object.stdout" 2> "$WORK_DIR/linker_macho_object.stderr" || linker_macho_object_status=$?
        assert_text_equals "$linker_macho_object_status" '1' "linker should reject Mach-O arm64 objects with the explicit Mach-O object boundary"
        assert_file_contains "$WORK_DIR/linker_macho_object.stderr" 'Mach-O arm64 relocatable object is recognized, but Mach-O linking is not implemented yet' "linker did not recognize a Mach-O arm64 relocatable before rejecting it"
    else
        echo "skipping Mach-O object recognizer test; clang could not emit arm64 Mach-O"
    fi
fi

printf 'BC\300\336' > "$WORK_DIR/clang_lto.bc"
linker_llvm_lto_status=0
"$LINKER" -m x86_64-linux -o "$WORK_DIR/clang_lto.bin" "$WORK_DIR/clang_lto.bc" > "$WORK_DIR/linker_llvm_lto.stdout" 2> "$WORK_DIR/linker_llvm_lto.stderr" || linker_llvm_lto_status=$?
assert_text_equals "$linker_llvm_lto_status" '1' "linker should reject LLVM bitcode without an LTO prelink compiler"
assert_file_contains "$WORK_DIR/linker_llvm_lto.stderr" 'LLVM/Clang LTO bitcode object; add --lto-cc=clang' "linker did not diagnose LLVM/Clang LTO bitcode distinctly"

CLANG_LTO_CC=${NEWOS_CLANG_LTO_CC:-clang}
if command -v "$CLANG_LTO_CC" >/dev/null 2>&1; then
    cat > "$WORK_DIR/clang_lto_start.c" <<'C'
__attribute__((noreturn)) void _start(void) {
    __asm__ volatile(
        "mov $60, %%rax\n"
        "xor %%rdi, %%rdi\n"
        "syscall\n"
        :
        :
        : "rax", "rdi", "memory");
    __builtin_unreachable();
}
C
    if "$CLANG_LTO_CC" -target x86_64-unknown-linux-elf -flto -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -fno-pic -fno-pie \
        -Oz -c "$WORK_DIR/clang_lto_start.c" -o "$WORK_DIR/clang_lto_start.o" > "$WORK_DIR/clang_lto_compile.out" 2>&1 && \
       "$CLANG_LTO_CC" -target x86_64-unknown-linux-elf -flto -fuse-ld=lld -r -nostdlib \
        -Wl,--gc-sections -Wl,-u,_start "$WORK_DIR/clang_lto_start.o" -o "$WORK_DIR/clang_lto_probe.o" > "$WORK_DIR/clang_lto_prelink_probe.out" 2>&1; then
        "$LINKER" --tiny --gc-sections --lto-cc="$CLANG_LTO_CC" -m x86_64-linux \
            -o "$WORK_DIR/clang_lto_start.bin" "$WORK_DIR/clang_lto_start.o" > "$WORK_DIR/linker_clang_lto.stdout" 2> "$WORK_DIR/linker_clang_lto.stderr"
        od -An -tx1 -N4 "$WORK_DIR/clang_lto_start.bin" | tr -d ' \n' > "$WORK_DIR/clang_lto_start.magic"
        assert_file_contains "$WORK_DIR/clang_lto_start.magic" '^7f454c46$' "Clang LTO prelink did not produce an ELF executable"
    else
        echo "skipping Clang LTO prelink test; $CLANG_LTO_CC cannot emit x86_64 ELF LTO with lld"
    fi
fi

echo "LINKER_CLI_OK"