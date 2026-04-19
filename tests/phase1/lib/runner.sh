#!/bin/sh
set -eu

: "${PHASE1_DIR:=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}"
ROOT_DIR=$(CDPATH= cd -- "$PHASE1_DIR/../.." && pwd)

. "$ROOT_DIR/tests/lib/assert.sh"

phase1_note() {
    note "Phase 1 :: $1"
}

phase1_list_scripts() {
    (
        cd "$PHASE1_DIR"
        find . -mindepth 2 -type f -name '*.sh' \
            ! -path './lib/*' \
            ! -name 'run_phase1_tests.sh' \
            | LC_ALL=C sort
    )
}

phase1_matches_filter() {
    rel_path=$1
    shift

    if [ "$#" -eq 0 ]; then
        return 0
    fi

    rel_path=${rel_path#./}
    for filter in "$@"; do
        case "$rel_path" in
            "$filter"|"$filter".sh|"$filter"/*|*/"$filter".sh)
                return 0
                ;;
        esac
    done

    return 1
}

phase1_run_script() {
    rel_path=$1
    display_name=${rel_path#./}
    display_name=${display_name%.sh}

    phase1_note "$display_name"
    sh "$PHASE1_DIR/$display_name.sh"
}

run_phase1_tests() {
    script_list=$(phase1_list_scripts)
    if [ -z "$script_list" ]; then
        phase1_note "no test groups discovered under tests/phase1"
        return 0
    fi

    status=0
    matched=0
    old_ifs=$IFS
    IFS='
'

    for rel_path in $script_list; do
        if phase1_matches_filter "$rel_path" "$@"; then
            matched=1
            if ! phase1_run_script "$rel_path"; then
                status=1
            fi
        fi
    done

    IFS=$old_ifs

    if [ "$matched" -eq 0 ]; then
        echo "No Phase 1 test groups matched: $*" >&2
        return 1
    fi

    return "$status"
}
