#!/bin/sh

newos_test_default_build_dir() {
    if [ -n "${NEWOS_TEST_BUILD_DIR:-}" ]; then
        printf '%s\n' "$NEWOS_TEST_BUILD_DIR"
        return 0
    fi
    if [ -n "${NEWOS_FREESTANDING_BUILD_DIR:-}" ]; then
        printf '%s\n' "$NEWOS_FREESTANDING_BUILD_DIR"
        return 0
    fi

    os_name=$(uname -s 2>/dev/null || echo unknown)
    arch_name=$(uname -m 2>/dev/null || echo unknown)
    case "$arch_name" in
        arm64)
            arch_name=aarch64
            ;;
    esac

    case "$os_name:$arch_name" in
        Darwin:aarch64)
            printf '%s\n' "$ROOT_DIR/build/newlinker-macos-aarch64"
            ;;
        Linux:*)
            printf '%s\n' "$ROOT_DIR/build/freestanding-linux-$arch_name"
            ;;
        MSYS_NT*:x86_64|MINGW*:x86_64|CYGWIN*:x86_64)
            printf '%s\n' "$ROOT_DIR/build/freestanding-windows-x86_64"
            ;;
        *)
            printf '%s\n' "$ROOT_DIR/build/freestanding-$os_name-$arch_name"
            ;;
    esac
}

newos_test_tool_dir() {
    newos_test_default_build_dir
}

newos_test_tool() {
    printf '%s/%s\n' "$(newos_test_tool_dir)" "$1"
}

newos_configure_test_tools() {
    TEST_BIN_DIR=$(newos_test_tool_dir)
    NEWOS_TEST_BUILD_DIR=$TEST_BIN_DIR
    NEWOS_FREESTANDING_BUILD_DIR=$TEST_BIN_DIR
    export TEST_BIN_DIR NEWOS_TEST_BUILD_DIR NEWOS_FREESTANDING_BUILD_DIR
}