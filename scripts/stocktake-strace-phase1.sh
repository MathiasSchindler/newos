#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PHASE1_DIR="$ROOT_DIR/tests/phase1"

. "$ROOT_DIR/tests/lib/build.sh"

BUILD_DIR=$(newos_test_default_build_dir)
STRACE_BIN=${NEWOS_STRACE_BIN:-$BUILD_DIR/strace}
OUT_DIR=${NEWOS_STRACE_STOCKTAKE_DIR:-$ROOT_DIR/tests/tmp/strace-phase1-stocktake}
CAPTURE_FILTER=${NEWOS_STRACE_STOCKTAKE_FILTER:-default}
BASELINE_ON_FAILURE=${NEWOS_STRACE_STOCKTAKE_BASELINE_ON_FAILURE:-1}
RAW_DIR="$OUT_DIR/raw"
SUMMARY_DIR="$OUT_DIR/summaries"
LOG_DIR="$OUT_DIR/logs"
BASELINE_LOG_DIR="$OUT_DIR/baseline-logs"

usage() {
    cat <<'USAGE'
Usage: scripts/stocktake-strace-phase1.sh [FILTER ...]

Run existing Phase 1 test groups with NEWOS_STRACE_FD set, then replay the
captured macOS project-linked trace records through strace -c.

Environment:
NEWOS_TEST_BUILD_DIR: real tool build directory to test.
NEWOS_STRACE_BIN: strace binary to use for record replay.
NEWOS_STRACE_STOCKTAKE_DIR: output directory, default tests/tmp/strace-phase1-stocktake.
NEWOS_STRACE_STOCKTAKE_FILTER: capture filter, default "default". Use "all" to include read/write records when a focused test group tolerates full tracing.
NEWOS_STRACE_STOCKTAKE_BASELINE_ON_FAILURE: rerun failed groups without tracing, default 1.
USAGE
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

safe_name_for_group() {
    printf '%s\n' "$1" | sed 's#[/.][/.]*#_#g; s#[^A-Za-z0-9_+-]#_#g'
}

append_summary_rows() {
    group=$1
    summary=$2

    awk -v group="$group" '
        NR == 1 { next }
        NF >= 5 { printf "%s\t%s\t%s\t%s\t%s\t%s\n", group, $1, $2, $3, $4, $5 }
    ' "$summary" >> "$OUT_DIR/syscall-summary.tsv"
}

write_aggregate_reports() {
    awk '
        NR == 1 { next }
        {
            calls[$2] += $3
            errors[$2] += $4
            bytes[$2] += $5
            total_ms[$2] += $6
        }
        END {
            for (name in calls) {
                printf "%s\t%d\t%d\t%d\t%.3f\n", name, calls[name], errors[name], bytes[name], total_ms[name]
            }
        }
    ' "$OUT_DIR/syscall-summary.tsv" | sort -k2,2nr > "$OUT_DIR/aggregate-by-syscall.tsv.tmp"
    printf 'syscall\tcalls\terrors\tbytes\ttotal_ms\n' > "$OUT_DIR/aggregate-by-syscall.tsv"
    cat "$OUT_DIR/aggregate-by-syscall.tsv.tmp" >> "$OUT_DIR/aggregate-by-syscall.tsv"
    rm -f "$OUT_DIR/aggregate-by-syscall.tsv.tmp"

    awk '
        NR == 1 { next }
        $2 == "write" && $5 > 0 { printf "write_calls_per_byte\t%s\t%s\t%s\t%.6f\n", $1, $3, $5, $3 / $5 }
        $2 == "read" && $5 > 0 { printf "read_calls_per_kib\t%s\t%s\t%s\t%.6f\n", $1, $3, $5, ($3 * 1024.0) / $5 }
        $4 > 0 { printf "syscall_errors\t%s\t%s\t%s\t%s\n", $1, $2, $4, $3 }
    ' "$OUT_DIR/syscall-summary.tsv" | sort -k5,5nr > "$OUT_DIR/hotspots.tsv"
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    usage
    exit 0
fi

if [ ! -x "$STRACE_BIN" ]; then
    echo "stocktake: strace not found or not executable: $STRACE_BIN" >&2
    exit 1
fi

rm -rf "$OUT_DIR"
mkdir -p "$RAW_DIR" "$SUMMARY_DIR" "$LOG_DIR" "$BASELINE_LOG_DIR"

printf 'group\tstatus\tbaseline_status\treplay_status\trecord_bytes\tlog\tbaseline_log\tsummary\trecords\n' > "$OUT_DIR/groups.tsv"
printf 'group\tsyscall\tcalls\terrors\tbytes\ttotal_ms\n' > "$OUT_DIR/syscall-summary.tsv"

matched=0
failed=0

for rel_path in $(phase1_list_scripts); do
    if ! phase1_matches_filter "$rel_path" "$@"; then
        continue
    fi
    matched=1
    group=${rel_path#./}
    group=${group%.sh}
    safe_group=$(safe_name_for_group "$group")
    record_path="$RAW_DIR/$safe_group.records"
    summary_path="$SUMMARY_DIR/$safe_group.summary"
    log_path="$LOG_DIR/$safe_group.log"
    baseline_log_path="$BASELINE_LOG_DIR/$safe_group.log"

    printf '==> strace stocktake :: %s\n' "$group"
    status=0
    baseline_status=-
    replay_status=0
    (
        export NEWOS_TEST_BUILD_DIR="$BUILD_DIR"
        export NEWOS_FREESTANDING_BUILD_DIR="$BUILD_DIR"
        export TEST_BIN_DIR="$BUILD_DIR"
        export NEWOS_STRACE_FD=9
        export NEWOS_STRACE_FILTER="$CAPTURE_FILTER"
        export NEWOS_STRACE_NO_METADATA=1
        sh "$PHASE1_DIR/$group.sh"
    ) 9> "$record_path" > "$log_path" 2>&1 || status=$?

    "$STRACE_BIN" -c --records "$record_path" -o "$summary_path" >> "$log_path" 2>&1 || replay_status=$?
    if [ "$replay_status" -ne 0 ]; then
        failed=1
    fi
    if [ -f "$summary_path" ]; then
        append_summary_rows "$group" "$summary_path"
    fi

    if [ "$status" -ne 0 ] && [ "$BASELINE_ON_FAILURE" != 0 ]; then
        baseline_status=0
        (
            export NEWOS_TEST_BUILD_DIR="$BUILD_DIR"
            export NEWOS_FREESTANDING_BUILD_DIR="$BUILD_DIR"
            export TEST_BIN_DIR="$BUILD_DIR"
            unset NEWOS_STRACE_FD
            unset NEWOS_STRACE_FILTER
            unset NEWOS_STRACE_NO_METADATA
            sh "$PHASE1_DIR/$group.sh"
        ) > "$baseline_log_path" 2>&1 || baseline_status=$?
    fi

    record_bytes=$(wc -c < "$record_path" | tr -d ' ')
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$group" "$status" "$baseline_status" "$replay_status" "$record_bytes" "$log_path" "$baseline_log_path" "$summary_path" "$record_path" >> "$OUT_DIR/groups.tsv"
    if [ "$status" -ne 0 ] && { [ "$baseline_status" = 0 ] || [ "$baseline_status" = - ]; }; then
        failed=1
    fi
done

if [ "$matched" -eq 0 ]; then
    echo "stocktake: no Phase 1 test groups matched: $*" >&2
    exit 1
fi

write_aggregate_reports

echo "stocktake output: $OUT_DIR"
echo "  groups:             $OUT_DIR/groups.tsv"
echo "  syscall summaries:  $OUT_DIR/syscall-summary.tsv"
echo "  aggregate:          $OUT_DIR/aggregate-by-syscall.tsv"
echo "  hotspots:           $OUT_DIR/hotspots.tsv"

exit "$failed"