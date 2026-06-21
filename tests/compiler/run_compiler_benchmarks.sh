#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CASE_DIR="$ROOT_DIR/tests/compiler/benchmarks"
WORK_DIR=${COMPILER_BENCH_WORK_DIR:-"$ROOT_DIR/tests/tmp/compiler-benchmarks"}
ITERATIONS=${COMPILER_BENCH_ITERATIONS:-5}
HOST_CC=${COMPILER_BENCH_HOST_CC:-cc}
NCC=${COMPILER_BENCH_NCC:-"$ROOT_DIR/build/host-linux-x86_64/ncc"}
TIME_BIN=${COMPILER_BENCH_TIME:-"$ROOT_DIR/build/freestanding-linux-x86_64/time"}
AWK_BIN=${AWK_BIN:-awk}

if [ "$(uname -s)" != "Linux" ] || [ "$(uname -m)" != "x86_64" ]; then
    printf 'compiler benchmarks are currently runnable only on Linux x86_64; skipping\n'
    exit 0
fi

if [ ! -x "$NCC" ]; then
    NCC="$ROOT_DIR/build/host-$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)/ncc"
fi
if [ ! -x "$NCC" ]; then
    NCC="$ROOT_DIR/build/ncc"
fi
if [ ! -x "$NCC" ]; then
    printf 'ncc not found; build it with make host or set COMPILER_BENCH_NCC\n' >&2
    exit 1
fi
if [ ! -x "$TIME_BIN" ]; then
    TIME_BIN="$ROOT_DIR/build/time"
fi
if [ ! -x "$TIME_BIN" ]; then
    TIME_BIN=""
fi

mkdir -p "$WORK_DIR"

measure_seconds() {
    command_text=$1
    total=0
    run=0

    while [ "$run" -lt "$ITERATIONS" ]; do
        log_file="$WORK_DIR/time.$$.$run.log"
        if [ -n "$TIME_BIN" ]; then
            "$TIME_BIN" sh -c "$command_text" >/dev/null 2>"$log_file"
            seconds=$(LC_ALL=C "$AWK_BIN" '/^real / { print $2 }' "$log_file")
        else
            LC_ALL=C /usr/bin/time -p sh -c "$command_text" >/dev/null 2>"$log_file"
            seconds=$(LC_ALL=C "$AWK_BIN" '/^real / { print $2 }' "$log_file")
        fi
        rm -f "$log_file"
        if [ -z "$seconds" ]; then
            seconds=0
        fi
        total=$(LC_ALL=C "$AWK_BIN" -v a="$total" -v b="$seconds" 'BEGIN { printf "%.6f", a + b }')
        run=$((run + 1))
    done

    LC_ALL=C "$AWK_BIN" -v total="$total" -v n="$ITERATIONS" 'BEGIN { if (n <= 0) printf "0.000000"; else printf "%.6f", total / n }'
}

file_size() {
    wc -c < "$1" | tr -d ' '
}

asm_lines() {
    if [ -f "$1" ]; then
        wc -l < "$1" | tr -d ' '
    else
        printf '0'
    fi
}

ratio() {
    LC_ALL=C "$AWK_BIN" -v a="$1" -v b="$2" 'BEGIN { if (b <= 0.0) printf "n/a"; else printf "%.2fx", a / b }'
}

printf "%-16s %-8s %-8s %-9s %-9s %-9s %-9s %-9s\n" \
    "case" "gcc-bytes" "ncc-bytes" "gcc-asm" "ncc-asm" "gcc-sec" "ncc-sec" "ncc/gcc"
printf "%-16s %-8s %-8s %-9s %-9s %-9s %-9s %-9s\n" \
    "----------------" "--------" "--------" "---------" "---------" "---------" "---------" "-------"

for source in "$CASE_DIR"/*.c; do
    case_name=$(basename "$source" .c)
    gcc_bin="$WORK_DIR/$case_name.gcc"
    ncc_bin="$WORK_DIR/$case_name.ncc"
    gcc_asm="$WORK_DIR/$case_name.gcc.s"
    ncc_asm="$WORK_DIR/$case_name.ncc.s"

    "$HOST_CC" -std=c11 -O2 "$source" -o "$gcc_bin"
    "$HOST_CC" -std=c11 -O2 -S "$source" -o "$gcc_asm"
    "$NCC" --target linux-x86_64 -O2 "$source" -o "$ncc_bin"
    "$NCC" --target linux-x86_64 -O2 -S "$source" -o "$ncc_asm"

    gcc_status=0
    ncc_status=0
    "$gcc_bin" >/dev/null 2>&1 || gcc_status=$?
    "$ncc_bin" >/dev/null 2>&1 || ncc_status=$?
    if [ "$gcc_status" -ne "$ncc_status" ]; then
        printf 'compiler benchmark status mismatch for %s: gcc=%s ncc=%s\n' "$case_name" "$gcc_status" "$ncc_status" >&2
        exit 1
    fi

    gcc_sec=$(measure_seconds "\"$gcc_bin\"")
    ncc_sec=$(measure_seconds "\"$ncc_bin\"")

    printf "%-16s %-8s %-8s %-9s %-9s %-9s %-9s %-9s\n" \
        "$case_name" \
        "$(file_size "$gcc_bin")" \
        "$(file_size "$ncc_bin")" \
        "$(asm_lines "$gcc_asm")" \
        "$(asm_lines "$ncc_asm")" \
        "$gcc_sec" \
        "$ncc_sec" \
        "$(ratio "$ncc_sec" "$gcc_sec")"
done

printf 'COMPILER_BENCHMARKS_OK\n'
