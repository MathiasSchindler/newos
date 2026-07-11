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
if ! grep -q '^icf exact sections/bytes: 1/' "$WORK_DIR/icf_reloc.stats"; then
    echo "linker stats did not attribute exact ICF savings" >&2
    exit 1
fi
if ! grep -q '^segment permissions: rx$' "$WORK_DIR/icf_reloc.stats"; then
    echo "linker stats did not report tiny read-only segment permissions" >&2
    exit 1
fi

cat > "$WORK_DIR/icf_recursive.s" <<'ASM'
.globl _start
_start:
    lea f1(%rip), %rax
    lea f2(%rip), %rbx
    cmp %rbx, %rax
    jne fail
    lea g1(%rip), %rax
    lea g2(%rip), %rbx
    cmp %rbx, %rax
    jne fail
    xor %rdi, %rdi
    jmp done
fail:
    mov $29, %rdi
done:
    mov $60, %rax
    syscall
.section .text.f1,"ax",@progbits
.globl f1
f1:
    call g1
    ret
.section .text.f2,"ax",@progbits
.globl f2
f2:
    call g2
    ret
.section .text.g1,"ax",@progbits
.globl g1
g1:
    nop
    call f1
    ret
.section .text.g2,"ax",@progbits
.globl g2
g2:
    nop
    call f2
    ret
ASM
cc -x assembler -c "$WORK_DIR/icf_recursive.s" -o "$WORK_DIR/icf_recursive.o"
"$LINKER" --tiny --gc-sections --icf=all --stats -m x86_64-linux \
    -o "$WORK_DIR/icf_recursive.bin" "$WORK_DIR/icf_recursive.o" > "$WORK_DIR/icf_recursive.stats"
"$WORK_DIR/icf_recursive.bin"
if ! grep -q '^icf equivalence sections/bytes: 2/' "$WORK_DIR/icf_recursive.stats"; then
    echo "equivalence-class ICF did not fold the mutually recursive groups" >&2
    exit 1
fi

cat > "$WORK_DIR/call_graph_order.s" <<'ASM'
.globl _start
.section .text.start,"ax",@progbits
_start:
    call first
    xor %rdi, %rdi
    mov $60, %rax
    syscall
.section .text.unrelated,"ax",@progbits
.globl unrelated
unrelated:
    ret
.section .text.second,"ax",@progbits
.globl second
second:
    ret
.section .text.first,"ax",@progbits
.globl first
first:
    call second
    ret
ASM
cc -x assembler -c "$WORK_DIR/call_graph_order.s" -o "$WORK_DIR/call_graph_order.o"
"$LINKER" --tiny --gc-sections --call-graph-order --map "$WORK_DIR/call_graph_order.map" \
    -m x86_64-linux -o "$WORK_DIR/call_graph_order.bin" "$WORK_DIR/call_graph_order.o"
"$WORK_DIR/call_graph_order.bin"
start_address=$(awk '$3 == ".text.start" { print $1 }' "$WORK_DIR/call_graph_order.map")
first_address=$(awk '$3 == ".text.first" { print $1 }' "$WORK_DIR/call_graph_order.map")
second_address=$(awk '$3 == ".text.second" { print $1 }' "$WORK_DIR/call_graph_order.map")
if ! (( start_address < first_address && first_address < second_address )); then
    echo "call-graph ordering did not place the entry path contiguously" >&2
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
"$LINKER" --tiny --gc-sections --icf=safe --stats -m x86_64-linux \
    -o "$WORK_DIR/merge_strings.bin" "$WORK_DIR/merge_strings.o" > "$WORK_DIR/merge_strings.stats"
"$WORK_DIR/merge_strings.bin"
string_count=$(grep -ao 'shared-tail' "$WORK_DIR/merge_strings.bin" | wc -l | tr -d ' ')
if [[ "$string_count" != "1" ]]; then
    echo "mergeable string pooling kept $string_count copies" >&2
    exit 1
fi
if ! grep -q '^merge strings input/output/saved: 29/12/17$' "$WORK_DIR/merge_strings.stats"; then
    echo "linker stats did not report mergeable string savings" >&2
    exit 1
fi

cat > "$WORK_DIR/merge_constants.s" <<'ASM'
.globl _start
_start:
    lea first(%rip), %rax
    lea duplicate(%rip), %rbx
    cmp %rbx, %rax
    jne fail_address
    cmpl $0x11223344, (%rax)
    jne fail_first
    lea other(%rip), %rax
    cmpl $0x55667788, (%rax)
    jne fail_other
    xor %rdi, %rdi
    jmp done
fail_address:
    mov $31, %rdi
    jmp done
fail_first:
    mov $32, %rdi
    jmp done
fail_other:
    mov $33, %rdi
done:
    mov $60, %rax
    syscall
.section .rodata.cst4,"aM",@progbits,4
first:
    .long 0x11223344
duplicate:
    .long 0x11223344
other:
    .long 0x55667788
ASM
cc -x assembler -c "$WORK_DIR/merge_constants.s" -o "$WORK_DIR/merge_constants.o"
"$LINKER" --tiny --gc-sections --merge-constants --stats -m x86_64-linux \
    -o "$WORK_DIR/merge_constants.bin" "$WORK_DIR/merge_constants.o" > "$WORK_DIR/merge_constants.stats"
"$WORK_DIR/merge_constants.bin"
if ! grep -q '^merge constants input/output/saved: 12/8/4$' "$WORK_DIR/merge_constants.stats"; then
    echo "linker stats did not report mergeable constant savings" >&2
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
