#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT_DIR/tests/lib/build.sh"
newos_configure_test_tools

LINKER=${NEWOS_LINKER:-$TEST_BIN_DIR/linker}
WORK_DIR=${TMPDIR:-/tmp}/newos-newlinker-optimizations-$$

cleanup() {
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

mkdir -p "$WORK_DIR"
cd "$ROOT_DIR"

if [[ ! -x "$LINKER" ]]; then
    echo "missing linker: $LINKER" >&2
    exit 1
fi
if ! command -v cc >/dev/null 2>&1; then
    echo "missing cc for assembler fixtures" >&2
    exit 1
fi
if command -v gcc >/dev/null 2>&1; then
    LTO_CC=${NEWOS_LTO_CC:-gcc}
else
    LTO_CC=${NEWOS_LTO_CC:-}
fi
bash -n scripts/build-freestanding-newlinker.sh

cat > "$WORK_DIR/icf_reloc.s" <<'ASM'
.globl _start
_start:
    lea f1(%rip), %rax
    lea f2(%rip), %rbx
    cmp %rbx, %rax
    jne fail
    call f1
    cmp $7, %rax
    jne fail
    xor %rdi, %rdi
    jmp done
fail:
    mov $17, %rdi
done:
    mov $60, %rax
    syscall
.section .text.f1,"ax",@progbits
.globl f1
.p2align 4
f1:
    call helper
    ret
.section .text.f2,"ax",@progbits
.globl f2
f2:
    call helper
    ret
.section .text.helper,"ax",@progbits
.globl helper
helper:
    mov $7, %rax
    ret
ASM
cc -x assembler -c "$WORK_DIR/icf_reloc.s" -o "$WORK_DIR/icf_reloc.o"
"$LINKER" --tiny --gc-sections --icf=safe --stats --map "$WORK_DIR/icf_reloc.map" \
    -m x86_64-linux -o "$WORK_DIR/icf_reloc.bin" "$WORK_DIR/icf_reloc.o" > "$WORK_DIR/icf_reloc.stats"
"$WORK_DIR/icf_reloc.bin"
if ! grep -q 'folded-to=' "$WORK_DIR/icf_reloc.map"; then
    echo "relocation-aware ICF did not fold the relocated fixture" >&2
    exit 1
fi
if ! grep -q 'linker stats' "$WORK_DIR/icf_reloc.stats"; then
    echo "linker stats output missing" >&2
    exit 1
fi

cat > "$WORK_DIR/merge_strings.s" <<'ASM'
.globl _start
_start:
    lea whole(%rip), %rsi
    cmpb $'s', 0(%rsi)
    jne fail
    lea duplicate(%rip), %rsi
    cmpb $'h', 1(%rsi)
    jne fail
    lea suffix(%rip), %rsi
    cmpb $'t', 0(%rsi)
    jne fail
    xor %rdi, %rdi
    jmp done
fail:
    mov $23, %rdi
done:
    mov $60, %rax
    syscall
.section .rodata.str1.1,"aMS",@progbits,1
whole:
    .asciz "shared-tail"
duplicate:
    .asciz "shared-tail"
suffix:
    .asciz "tail"
ASM
cc -x assembler -c "$WORK_DIR/merge_strings.s" -o "$WORK_DIR/merge_strings.o"
"$LINKER" --tiny --gc-sections --icf=safe -m x86_64-linux \
    -o "$WORK_DIR/merge_strings.bin" "$WORK_DIR/merge_strings.o"
"$WORK_DIR/merge_strings.bin"
string_count=$(grep -ao 'shared-tail' "$WORK_DIR/merge_strings.bin" | wc -l | tr -d ' ')
if [[ "$string_count" != "1" ]]; then
    echo "mergeable string pooling kept $string_count copies" >&2
    exit 1
fi

cat > "$WORK_DIR/tiny_minimal.s" <<'ASM'
.globl _start
_start:
    xor %rdi, %rdi
    mov $60, %rax
    syscall
ASM
cc -x assembler -c "$WORK_DIR/tiny_minimal.s" -o "$WORK_DIR/tiny_minimal.o"
"$LINKER" --tiny --gc-sections -m x86_64-linux \
    -o "$WORK_DIR/tiny_minimal.bin" "$WORK_DIR/tiny_minimal.o"
"$WORK_DIR/tiny_minimal.bin"
tiny_minimal_size=$(wc -c < "$WORK_DIR/tiny_minimal.bin")
if [[ "$tiny_minimal_size" -ge 140 ]]; then
    echo "tiny layout kept unnecessary text padding: $tiny_minimal_size bytes" >&2
    exit 1
fi

if [[ -n "$LTO_CC" ]]; then
    cat > "$WORK_DIR/gcc_lto_start.c" <<'C'
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
    "$LTO_CC" -flto -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-unwind-tables -fno-asynchronous-unwind-tables \
        -ffunction-sections -fdata-sections -fno-pic -fno-pie \
        -c "$WORK_DIR/gcc_lto_start.c" -o "$WORK_DIR/gcc_lto_start.o"
    if ! grep -aq '\.gnu\.lto_' "$WORK_DIR/gcc_lto_start.o"; then
        echo "GCC LTO fixture did not contain .gnu.lto_* sections" >&2
        exit 1
    fi
    if "$LINKER" --tiny --gc-sections -m x86_64-linux \
        -o "$WORK_DIR/gcc_lto_missing.bin" "$WORK_DIR/gcc_lto_start.o" \
        > "$WORK_DIR/gcc_lto_missing.out" 2>&1; then
        echo "GCC LTO fixture linked without --lto-cc" >&2
        exit 1
    fi
    if ! grep -q -- '--lto-cc=gcc' "$WORK_DIR/gcc_lto_missing.out"; then
        echo "GCC LTO fixture did not report the --lto-cc requirement" >&2
        exit 1
    fi
    "$LINKER" --tiny --gc-sections --lto-cc="$LTO_CC" -m x86_64-linux \
        -o "$WORK_DIR/gcc_lto_start.bin" "$WORK_DIR/gcc_lto_start.o"
    "$WORK_DIR/gcc_lto_start.bin"
fi

echo "NEWLINKER_OPTIMIZATIONS_OK"
