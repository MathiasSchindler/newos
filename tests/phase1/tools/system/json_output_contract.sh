#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)

. "$ROOT_DIR/tests/lib/assert.sh"

TMP_DIR="$ROOT_DIR/tests/tmp/json-output-contract"
rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR"

tools=$(sed -n 's/^TOOLS := //p' "$ROOT_DIR/Makefile")

status=0
for tool in $tools; do
    man_page="$ROOT_DIR/man/1/$tool.md"
    if [ ! -f "$man_page" ]; then
        echo "missing man page for tool: $tool" >&2
        status=1
        continue
    fi

    if ! grep '^## JSON Output$' "$man_page" >/dev/null 2>&1; then
        echo "missing JSON Output section: man/1/$tool.md" >&2
        status=1
        continue
    fi

    tool_path="$ROOT_DIR/build/$tool"
    if [ -x "$tool_path" ]; then
        if "$tool_path" --json --help >"$TMP_DIR/$tool.out" 2>"$TMP_DIR/$tool.err"; then
            continue
        fi
    fi

    if ! grep 'JSON mode limitation' "$man_page" >/dev/null 2>&1; then
        echo "tool neither accepts --json --help nor documents a JSON mode limitation: $tool" >&2
        status=1
    fi
done

exit "$status"
