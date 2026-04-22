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

phase1_detect_jobs() {
    jobs=${PHASE1_JOBS:-}

    if [ -z "$jobs" ]; then
        jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
    fi

    case "$jobs" in
        ''|*[!0-9]*)
            jobs=4
            ;;
    esac

    if [ "$jobs" -lt 1 ]; then
        jobs=1
    fi
    if [ "$jobs" -gt 8 ]; then
        jobs=8
    fi

    printf '%s\n' "$jobs"
}

phase1_wait_for_any() {
    job_dir=$1
    max_index=$2

    while :; do
        idx=1
        while [ "$idx" -le "$max_index" ]; do
            if [ ! -f "$job_dir/$idx.done" ] && [ -f "$job_dir/$idx.pid" ]; then
                pid=$(cat "$job_dir/$idx.pid")
                display_name=
                if [ -f "$job_dir/$idx.name" ]; then
                    display_name=$(cat "$job_dir/$idx.name")
                fi
                if ! kill -0 "$pid" 2>/dev/null; then
                    if wait "$pid"; then
                        rc=0
                    else
                        rc=$?
                    fi
                    printf '%s\n' "$rc" > "$job_dir/$idx.status"
                    : > "$job_dir/$idx.done"
                    phase1_done_count=$((phase1_done_count + 1))
                    if [ -n "$display_name" ]; then
                        if [ "$rc" -eq 0 ]; then
                            note "Phase 1 completed [$phase1_done_count]: $display_name"
                        else
                            note "Phase 1 completed [$phase1_done_count]: $display_name (failed)"
                        fi
                    fi
                    return 0
                fi
            fi
            idx=$((idx + 1))
        done

        sleep 1
    done
}

phase1_replay_logs() {
    job_dir=$1
    max_index=$2
    status=0
    idx=1

    while [ "$idx" -le "$max_index" ]; do
        if [ -f "$job_dir/$idx.log" ]; then
            cat "$job_dir/$idx.log"
        fi
        if [ -f "$job_dir/$idx.status" ]; then
            rc=$(cat "$job_dir/$idx.status")
            if [ "$rc" -ne 0 ]; then
                status=1
            fi
        else
            status=1
        fi
        idx=$((idx + 1))
    done

    return "$status"
}

run_phase1_tests() {
    script_list=$(phase1_list_scripts)
    if [ -z "$script_list" ]; then
        phase1_note "no test groups discovered under tests/phase1"
        return 0
    fi

    matched=0
    selected=0
    phase1_done_count=0
    max_jobs=$(phase1_detect_jobs)
    job_dir="$ROOT_DIR/tests/tmp/phase1_runner"
    old_ifs=$IFS
    IFS='
'

    rm -rf "$job_dir"
    mkdir -p "$job_dir"

    note "Phase 1 runner using up to $max_jobs job(s)"

    for rel_path in $script_list; do
        if phase1_matches_filter "$rel_path" "$@"; then
            matched=1
            selected=$((selected + 1))

            if [ "$max_jobs" -le 1 ]; then
                if ! phase1_run_script "$rel_path"; then
                    printf '%s\n' 1 > "$job_dir/$selected.status"
                else
                    printf '%s\n' 0 > "$job_dir/$selected.status"
                fi
                continue
            fi

            display_name=${rel_path#./}
            display_name=${display_name%.sh}
            (
                phase1_run_script "$rel_path"
            ) > "$job_dir/$selected.log" 2>&1 &
            printf '%s\n' "$!" > "$job_dir/$selected.pid"
            printf '%s\n' "$display_name" > "$job_dir/$selected.name"

            while [ $((selected - phase1_done_count)) -ge "$max_jobs" ]; do
                phase1_wait_for_any "$job_dir" "$selected"
            done
        fi
    done

    IFS=$old_ifs

    if [ "$matched" -eq 0 ]; then
        echo "No Phase 1 test groups matched: $*" >&2
        return 1
    fi

    if [ "$max_jobs" -le 1 ]; then
        status=0
        idx=1
        while [ "$idx" -le "$selected" ]; do
            rc=$(cat "$job_dir/$idx.status")
            if [ "$rc" -ne 0 ]; then
                status=1
            fi
            idx=$((idx + 1))
        done
        return "$status"
    fi

    while [ "$phase1_done_count" -lt "$selected" ]; do
        phase1_wait_for_any "$job_dir" "$selected"
    done

    phase1_replay_logs "$job_dir" "$selected"
}
