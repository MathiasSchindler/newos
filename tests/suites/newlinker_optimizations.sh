#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
LINKER=${NEWOS_LINKER:-$ROOT_DIR/build/host-linux-x86_64/linker}
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
bash -n build-freestanding-newlinker.sh

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

echo "NEWLINKER_OPTIMIZATIONS_OK"
