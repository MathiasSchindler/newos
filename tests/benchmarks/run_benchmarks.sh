#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"

WORK_DIR="$ROOT_DIR/tests/tmp/benchmarks"
DATA_DIR="$WORK_DIR/data"
ITERATIONS=${BENCH_ITERATIONS:-3}
AWK_BIN=${AWK_BIN:-/usr/bin/awk}

if [ ! -x "$AWK_BIN" ]; then
    AWK_BIN=awk
fi

export ROOT_DIR WORK_DIR DATA_DIR ITERATIONS AWK_BIN

mkdir -p "$DATA_DIR"

create_data() {
    note "preparing benchmark data"

    awk 'BEGIN { for (i = 1800; i >= 1; --i) printf "%08d key-%04d repeated benchmark payload for sorting and hashing\n", i, (i % 1000) }' > "$DATA_DIR/sort_input.txt"

    : > "$DATA_DIR/compress_input.txt"
    repeat=0
    while [ "$repeat" -lt 96 ]; do
        cat "$DATA_DIR/sort_input.txt" >> "$DATA_DIR/compress_input.txt"
        repeat=$((repeat + 1))
    done
}

measure_seconds() {
    command_text="$1"
    total="0"
    run=0

    while [ "$run" -lt "$ITERATIONS" ]; do
        log_file="$WORK_DIR/time.$$.$run.log"
        LC_ALL=C /usr/bin/time -p sh -c "$command_text" >/dev/null 2>"$log_file"
        seconds=$(LC_ALL=C "$AWK_BIN" '/^real / { print $2 }' "$log_file")
        rm -f "$log_file"
        if [ -z "$seconds" ]; then
            seconds="0"
        fi
        total=$(LC_ALL=C "$AWK_BIN" -v a="$total" -v b="$seconds" 'BEGIN { printf "%.6f", a + b }')
        run=$((run + 1))
    done

    LC_ALL=C "$AWK_BIN" -v total="$total" -v n="$ITERATIONS" 'BEGIN { if (n <= 0) printf "0.0000"; else printf "%.4f", total / n }'
}

print_header() {
    printf "%-14s %-10s %-10s %s\n" "benchmark" "newos(s)" "system(s)" "ratio"
    printf "%-14s %-10s %-10s %s\n" "--------------" "----------" "----------" "-----"
}

print_result() {
    label="$1"
    ours="$2"
    system="$3"
    ours_norm=$(printf '%s' "$ours" | tr ',' '.')
    system_norm=$(printf '%s' "$system" | tr ',' '.')

    case "$system_norm" in
        ""|0|0.0|0.00|0.000|0.0000|0.00000|0.000000)
            ratio="n/a"
            ;;
        *)
            ratio=$(LC_ALL=C "$AWK_BIN" -v a="$ours_norm" -v b="$system_norm" 'BEGIN { if (b <= 0.0) print "n/a"; else printf "%.2fx", a / b }')
            ;;
    esac

    printf "%-14s %-10s %-10s %s\n" "$label" "$ours" "$system" "$ratio"
}

benchmark_sort() {
    ours=$(measure_seconds '"$ROOT_DIR"/build/sort "'$DATA_DIR'/sort_input.txt" > "'$WORK_DIR'/sort.out"')
    system=$(measure_seconds 'LC_ALL=C sort "'$DATA_DIR'/sort_input.txt" > "'$WORK_DIR'/sort.sys.out"')
    assert_files_equal "$WORK_DIR/sort.out" "$WORK_DIR/sort.sys.out" "sort benchmark output mismatch"
    print_result sort "$ours" "$system"
}

benchmark_hashes() {
    if command -v md5 >/dev/null 2>&1; then
        ours=$(measure_seconds '"$ROOT_DIR"/build/md5sum "'$DATA_DIR'/compress_input.txt" > /dev/null')
        system=$(measure_seconds 'md5 -q "'$DATA_DIR'/compress_input.txt" > /dev/null')
        print_result md5 "$ours" "$system"
    elif command -v md5sum >/dev/null 2>&1; then
        ours=$(measure_seconds '"$ROOT_DIR"/build/md5sum "'$DATA_DIR'/compress_input.txt" > /dev/null')
        system=$(measure_seconds 'md5sum "'$DATA_DIR'/compress_input.txt" > /dev/null')
        print_result md5 "$ours" "$system"
    fi

    if command -v shasum >/dev/null 2>&1; then
        ours=$(measure_seconds '"$ROOT_DIR"/build/sha256sum "'$DATA_DIR'/compress_input.txt" > /dev/null')
        system=$(measure_seconds 'shasum -a 256 "'$DATA_DIR'/compress_input.txt" > /dev/null')
        print_result sha256 "$ours" "$system"

        ours=$(measure_seconds '"$ROOT_DIR"/build/sha512sum "'$DATA_DIR'/compress_input.txt" > /dev/null')
        system=$(measure_seconds 'shasum -a 512 "'$DATA_DIR'/compress_input.txt" > /dev/null')
        print_result sha512 "$ours" "$system"
    fi
}

benchmark_compression() {
    ours=$(measure_seconds 'cp "'$DATA_DIR'/compress_input.txt" "'$WORK_DIR'/gzip-input.txt" && "'$ROOT_DIR'/build/gzip" "'$WORK_DIR'/gzip-input.txt" >/dev/null 2>&1 && rm -f "'$WORK_DIR'/gzip-input.txt" "'$WORK_DIR'/gzip-input.txt.gz"')
    system=$(measure_seconds 'gzip -c "'$DATA_DIR'/compress_input.txt" > "'$WORK_DIR'/gzip.sys.gz" && rm -f "'$WORK_DIR'/gzip.sys.gz"')
    print_result gzip "$ours" "$system"

    ours=$(measure_seconds 'cp "'$DATA_DIR'/compress_input.txt" "'$WORK_DIR'/bzip-input.txt" && "'$ROOT_DIR'/build/bzip2" "'$WORK_DIR'/bzip-input.txt" >/dev/null 2>&1 && rm -f "'$WORK_DIR'/bzip-input.txt" "'$WORK_DIR'/bzip-input.txt.bz2"')
    system=$(measure_seconds 'bzip2 -c "'$DATA_DIR'/compress_input.txt" > "'$WORK_DIR'/bzip.sys.bz2" && rm -f "'$WORK_DIR'/bzip.sys.bz2"')
    print_result bzip2 "$ours" "$system"

    if command -v xz >/dev/null 2>&1; then
        ours=$(measure_seconds 'cp "'$DATA_DIR'/compress_input.txt" "'$WORK_DIR'/xz-input.txt" && "'$ROOT_DIR'/build/xz" "'$WORK_DIR'/xz-input.txt" >/dev/null 2>&1 && rm -f "'$WORK_DIR'/xz-input.txt" "'$WORK_DIR'/xz-input.txt.xz"')
        system=$(measure_seconds 'xz -c "'$DATA_DIR'/compress_input.txt" > "'$WORK_DIR'/xz.sys.xz" && rm -f "'$WORK_DIR'/xz.sys.xz"')
        print_result xz "$ours" "$system"
    fi
}

create_data
print_header
benchmark_sort
benchmark_hashes
benchmark_compression

echo "ALL_BENCHMARKS_OK"
