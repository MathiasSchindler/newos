#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup runtime_macos_clock

case "$(uname -s):$(uname -m)" in
    Darwin:arm64|Darwin:aarch64) ;;
    *) note "macOS ARM64 clock regression requires a native macOS ARM64 host"; exit 0 ;;
esac
if ! command -v xcrun >/dev/null 2>&1; then
    note "macOS clock regression requires the Apple Clang toolchain"
    exit 0
fi
CLANG=$(xcrun --find clang)

cat > "$WORK_DIR/gettimeofday_stale_x2.S" <<'ASM'
.globl __start
.p2align 2
__start:
    sub sp, sp, #16
    mov x0, sp
    mov x1, #0
    mov x2, #2
    bl _gettimeofday
    cbnz w0, 1f
    ldr x8, [sp]
    cmp x8, #0
    b.le 1f
    ldr w8, [sp, #8]
    mov w9, #0x4240
    movk w9, #0xf, lsl #16
    cmp w8, w9
    b.hs 1f
    mov x0, #0
    b 2f
1:
    mov x0, #1
2:
    mov x16, #1
    svc #0x80
ASM
"$CLANG" -target arm64-apple-macos11 -c "$WORK_DIR/gettimeofday_stale_x2.S" -o "$WORK_DIR/gettimeofday_stale_x2.o"
make --no-print-directory -C "$ROOT_DIR" -f - CLOCK_TEST_DIR="$WORK_DIR" "$WORK_DIR/gettimeofday_stale_x2" <<'MAKE'
include Makefile

$(CLOCK_TEST_DIR)/gettimeofday_stale_x2: $(CLOCK_TEST_DIR)/gettimeofday_stale_x2.o $(MACOS_COMMON_OBJECTS) $(BUILD_DIR)/linker
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $< $(MACOS_COMMON_OBJECTS)
MAKE
assert_command_succeeds "$WORK_DIR/gettimeofday_stale_x2"
