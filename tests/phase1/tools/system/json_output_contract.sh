#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)

. "$ROOT_DIR/tests/lib/assert.sh"

note "phase1 system: json output contract"

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

    if grep 'JSON mode limitation' "$man_page" >/dev/null 2>&1; then
        continue
    fi

    source_path="$ROOT_DIR/src/tools/$tool.c"
    case "$tool" in
        '[')
            source_path="$ROOT_DIR/src/tools/[.c"
            ;;
        ping6)
            source_path="$ROOT_DIR/src/tools/ping.c"
            ;;
    esac

    if [ ! -f "$source_path" ]; then
        echo "tool has JSON Output section without limitation but no source file evidence: $tool" >&2
        status=1
        continue
    fi

    if ! grep -- '--json\|tool_opt_next' "$source_path" >/dev/null 2>&1; then
        echo "tool has JSON Output section without limitation but no static --json parser evidence: $tool" >&2
        status=1
    fi
done

exit "$status"
