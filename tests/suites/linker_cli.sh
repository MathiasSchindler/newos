#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"

LINKER=${NEWOS_LINKER:-$ROOT_DIR/build/linker}
WORK_DIR="$ROOT_DIR/tests/tmp/linker_cli"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

note "linker cli"

if [ ! -x "$LINKER" ]; then
    fail "missing linker: $LINKER"
fi

linker_macho_status=0
"$LINKER" --target=mach-o-arm64 -o "$WORK_DIR/linker_macho.out" "$WORK_DIR/missing.o" > "$WORK_DIR/linker_macho.stdout" 2> "$WORK_DIR/linker_macho.stderr" || linker_macho_status=$?
assert_text_equals "$linker_macho_status" '1' "linker Mach-O target should be a named but unimplemented backend"
assert_file_contains "$WORK_DIR/linker_macho.stderr" 'Mach-O arm64 linker backend is not implemented yet' "linker Mach-O target did not report the backend boundary"

printf 'BC\300\336' > "$WORK_DIR/clang_lto.bc"
linker_llvm_lto_status=0
"$LINKER" -m x86_64-linux -o "$WORK_DIR/clang_lto.bin" "$WORK_DIR/clang_lto.bc" > "$WORK_DIR/linker_llvm_lto.stdout" 2> "$WORK_DIR/linker_llvm_lto.stderr" || linker_llvm_lto_status=$?
assert_text_equals "$linker_llvm_lto_status" '1' "linker should reject LLVM bitcode without an LTO prelink compiler"
assert_file_contains "$WORK_DIR/linker_llvm_lto.stderr" 'LLVM/Clang LTO bitcode object; add --lto-cc=clang' "linker did not diagnose LLVM/Clang LTO bitcode distinctly"

echo "LINKER_CLI_OK"