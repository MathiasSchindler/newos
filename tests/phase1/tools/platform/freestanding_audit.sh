#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "platform/freestanding_audit"
note "phase1 platform: freestanding audit"

hosted_guard_files=$(
    find "$ROOT_DIR/src" -type f \( -name '*.c' -o -name '*.h' \) -exec grep -l '__STDC_HOSTED__' {} + |
        sed "s|$ROOT_DIR/||" |
        LC_ALL=C sort
)
unexpected_hosted=""
for rel in $hosted_guard_files; do
    case "$rel" in
        src/compiler/driver.c|\
        src/platform/*|\
        src/shared/shell_builtins.c|\
        src/shared/shell_interactive.c|\
        src/shared/tool_util.c|\
        src/tools/date.c|\
        src/tools/env.c|\
        src/tools/less.c|\
        src/tools/mktemp.c|\
        src/tools/more.c|\
        src/tools/printenv.c|\
        src/tools/sort.c|\
        src/tools/sh.c|\
        src/tools/test_impl.h|\
        src/tools/timeout.c|\
        src/tools/which.c)
            ;;
        *)
            unexpected_hosted="${unexpected_hosted}${unexpected_hosted:+ }$rel"
            ;;
    esac
done
[ -z "$unexpected_hosted" ] || fail "unexpected __STDC_HOSTED__ usage outside the approved platform boundary: $unexpected_hosted"

hosted_header_hits=$(
    find "$ROOT_DIR/src" -type f \( -name '*.c' -o -name '*.h' \) \
        -exec grep -nH -E '#include <(arpa/inet\.h|netdb\.h|sys/socket\.h|termios\.h|sys/wait\.h|utmpx\.h|sys/sysctl\.h)>' {} + || true
)
unexpected_headers=""
while IFS= read -r hit; do
    [ -n "$hit" ] || continue
    rel=${hit#"$ROOT_DIR"/}
    case "$rel" in
        src/platform/*:*|\
        src/shared/shell_interactive.c:*|\
        src/tools/timeout.c:*)
            ;;
        *)
            unexpected_headers="${unexpected_headers}${unexpected_headers:+; }$rel"
            ;;
    esac
done <<EOF
$hosted_header_hits
EOF
[ -z "$unexpected_headers" ] || fail "unexpected hosted-only headers escaped the platform layer: $unexpected_headers"

if [ "$(uname -s)" = 'Linux' ] && [ "$(uname -m)" = 'x86_64' ]; then
    note "phase1 platform: native freestanding build"
    assert_command_succeeds make -C "$ROOT_DIR" --no-print-directory freestanding
    [ -x "$ROOT_DIR/build/linux-x86_64/echo" ] || fail "linux/x86-64 freestanding echo binary was not built"
    "$ROOT_DIR/build/linux-x86_64/echo" freestanding > "$WORK_DIR/freestanding_echo.out"
    assert_file_contains "$WORK_DIR/freestanding_echo.out" '^freestanding$' "linux/x86-64 freestanding echo did not run correctly"
fi
