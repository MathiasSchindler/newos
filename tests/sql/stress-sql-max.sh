#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
SQL_BIN="$ROOT/build/sql"
TMP_DIR=${SQL_STRESS_TMP:-"$ROOT/tests/tmp/sql-stress"}
ROWS=${SQL_STRESS_ROWS:-40000}
ROW_LIMIT=${SQL_STRESS_ROW_LIMIT:-1048576}
COLS=${SQL_STRESS_COLS:-32}
GROUP_ROWS=32
KEEP=${SQL_STRESS_KEEP:-0}
TIME_MODE=none

DB="$TMP_DIR/stress.db"
BIG_TSV="$TMP_DIR/big.tsv"
GROUP_TSV="$TMP_DIR/groups.tsv"
EXPECTED="$TMP_DIR/expected.out"
ACTUAL="$TMP_DIR/actual.out"
META="$TMP_DIR/time.out"

die() {
    echo "error: $*" >&2
    exit 1
}

validate_number() {
    name=$1
    value=$2
    case "$value" in
        ''|*[!0-9]*) die "$name must be a positive integer" ;;
    esac
}

validate_number SQL_STRESS_ROWS "$ROWS"
validate_number SQL_STRESS_ROW_LIMIT "$ROW_LIMIT"
validate_number SQL_STRESS_COLS "$COLS"

[ "$ROWS" -ge 1 ] || die "SQL_STRESS_ROWS must be at least 1"
[ "$ROWS" -le "$ROW_LIMIT" ] || die "SQL_STRESS_ROWS must be at most SQL_STRESS_ROW_LIMIT ($ROW_LIMIT)"
[ "$COLS" -ge 4 ] || die "SQL_STRESS_COLS must be at least 4"
[ "$COLS" -le 1024 ] || die "SQL_STRESS_COLS must be at most 1024"

mkdir -p "$TMP_DIR"
cd "$ROOT"
make host >/dev/null

if [ "$(uname -s 2>/dev/null || echo unknown)" = Darwin ]; then
    TIME_MODE=darwin
elif /usr/bin/time -v true >/dev/null 2>&1; then
    TIME_MODE=gnu
fi

rm -f "$DB" "$BIG_TSV" "$GROUP_TSV" "$EXPECTED" "$ACTUAL" "$META"

column_list() {
    prefix=$1
    count=$2
    result=""
    index=1
    while [ "$index" -le "$count" ]; do
        if [ "$index" -gt 1 ]; then
            result="$result, "
        fi
        result="$result$prefix$index"
        index=$((index + 1))
    done
    printf '%s' "$result"
}

generate_big_tsv() {
    awk -v rows="$ROWS" -v cols="$COLS" 'BEGIN {
        for (c = 1; c <= cols; c++) {
            if (c > 1) {
                printf "\t"
            }
            printf "c%d", c
        }
        printf "\n"
        for (r = 1; r <= rows; r++) {
            for (c = 1; c <= cols; c++) {
                if (c > 1) {
                    printf "\t"
                }
                if (c == 1) {
                    printf "%d", r
                } else if (c == 2) {
                    printf "%d", (r % 32) + 1
                } else if (c == 3) {
                    printf "%d", ((r * 17) % 9000) + 1000
                } else {
                    printf "%013d", ((r * 1103515245 + c * 12345 + 67890) % 1000000000)
                }
            }
            printf "\n"
        }
    }' >"$BIG_TSV"
}

generate_group_tsv() {
    awk -v rows="$GROUP_ROWS" -v cols="$COLS" 'BEGIN {
        printf "group_id"
        for (c = 2; c <= cols; c++) {
            printf "\tg%d", c
        }
        printf "\n"
        for (r = 1; r <= rows; r++) {
            printf "%d", r
            for (c = 2; c <= cols; c++) {
                printf "\tg%d_%d", r, c
            }
            printf "\n"
        }
    }' >"$GROUP_TSV"
}

rss_summary() {
    meta=$1
    system_name=$(uname -s 2>/dev/null || echo unknown)
    if [ "$system_name" = Darwin ]; then
        rss_bytes=$(awk '/maximum resident set size/ {print $1}' "$meta" | tail -1)
        if [ -n "$rss_bytes" ]; then
            awk -v bytes="$rss_bytes" 'BEGIN { printf "%.1f MiB", bytes / 1048576 }'
            return
        fi
    else
        rss_kib=$(awk -F: '/Maximum resident set size/ {gsub(/^[ 	]+/, "", $2); print $2}' "$meta" | tail -1)
        if [ -n "$rss_kib" ]; then
            awk -v kib="$rss_kib" 'BEGIN { printf "%.1f MiB", kib / 1024 }'
            return
        fi
    fi
    printf 'unavailable'
}

elapsed_summary() {
    meta=$1
    system_name=$(uname -s 2>/dev/null || echo unknown)
    if [ "$system_name" = Darwin ]; then
        real_seconds=$(awk '$2 == "real" {print $1}' "$meta" | tail -1)
        if [ -n "$real_seconds" ]; then
            printf '%ss' "$real_seconds"
            return
        fi
    else
        elapsed=$(awk '/Elapsed \(wall clock\) time/ {sub(/^.*: /, ""); print}' "$meta" | tail -1)
        if [ -n "$elapsed" ]; then
            printf '%s' "$elapsed"
            return
        fi
    fi
    printf 'unavailable'
}

run_timed_sql() {
    label=$1
    sql=$2
    expected=$3
    actual="$TMP_DIR/$label.actual"
    meta="$TMP_DIR/$label.time"

    rm -f "$actual" "$meta"
    case "$TIME_MODE" in
        darwin)
            /usr/bin/time -l "$SQL_BIN" "$DB" "$sql" >"$actual" 2>"$meta" || {
                echo "not ok - $label" >&2
                cat "$meta" >&2 || true
                exit 1
            }
            ;;
        gnu)
            /usr/bin/time -v "$SQL_BIN" "$DB" "$sql" >"$actual" 2>"$meta" || {
                echo "not ok - $label" >&2
                cat "$meta" >&2 || true
                exit 1
            }
            ;;
        *)
            "$SQL_BIN" "$DB" "$sql" >"$actual" 2>"$meta" || {
                echo "not ok - $label" >&2
                cat "$meta" >&2 || true
                exit 1
            }
            ;;
    esac

    if [ -n "$expected" ] && ! diff -u "$expected" "$actual"; then
        echo "not ok - $label" >&2
        cat "$meta" >&2 || true
        exit 1
    fi

    printf '%-24s %-12s elapsed: %s\n' "$label peak RSS:" "$(rss_summary "$meta")" "$(elapsed_summary "$meta")"
}

target_group=$((ROWS % GROUP_ROWS + 1))
target_amount=$(((ROWS * 17) % 9000 + 1000))
target_tail=$(awk -v row="$ROWS" -v col="$COLS" 'BEGIN { printf "%013d", ((row * 1103515245 + col * 12345 + 67890) % 1000000000) }')
target_group_count=$(awk -v rows="$ROWS" -v group="$target_group" 'BEGIN {
    count = 0
    for (r = 1; r <= rows; r++) {
        if ((r % 32) + 1 == group) {
            count++
        }
    }
    printf "%d", count
}')

printf 'generating %s rows x %s columns...\n' "$ROWS" "$COLS"
generate_big_tsv
generate_group_tsv

big_columns=$(column_list c "$COLS")
group_columns="group_id"
group_col=2
while [ "$group_col" -le "$COLS" ]; do
    group_columns="$group_columns, g$group_col"
    group_col=$((group_col + 1))
done

create_and_import="CREATE TABLE big($big_columns); CREATE TABLE groups($group_columns); IMPORT big FROM '$BIG_TSV'; IMPORT groups FROM '$GROUP_TSV';"

{
    echo "ok"
    echo "ok"
    echo "$ROWS rows"
    echo "$GROUP_ROWS rows"
} >"$EXPECTED"

printf 'building stress database...\n'
run_timed_sql create-import "$create_and_import" "$EXPECTED"

db_bytes=$(wc -c <"$DB" | tr -d ' ')
big_tsv_bytes=$(wc -c <"$BIG_TSV" | tr -d ' ')
printf 'database size:          %s bytes\n' "$db_bytes"
printf 'source TSV size:        %s bytes\n' "$big_tsv_bytes"

{
    echo "total"
    echo "$ROWS"
} >"$EXPECTED"
run_timed_sql count "SELECT COUNT(*) AS total FROM big;" "$EXPECTED"

{
    echo "id\tgroup_id\tamount\ttail"
    echo "$ROWS\t$target_group\t$target_amount\t$target_tail"
} >"$EXPECTED"
run_timed_sql point-lookup "SELECT c1 AS id, c2 AS group_id, c3 AS amount, c$COLS AS tail FROM big WHERE c1 = $ROWS;" "$EXPECTED"

{
    echo "group_id\ttotal"
    echo "$target_group\t$target_group_count"
} >"$EXPECTED"
run_timed_sql grouped-count "SELECT c2 AS group_id, COUNT(*) AS total FROM big WHERE c2 = $target_group GROUP BY c2;" "$EXPECTED"

{
    echo "id\tgroup_id\tlookup"
    echo "$ROWS\t$target_group\tg${target_group}_3"
} >"$EXPECTED"
run_timed_sql join-lookup "SELECT b.c1 AS id, b.c2 AS group_id, g.g3 AS lookup FROM big b JOIN groups g ON b.c2 = g.group_id WHERE b.c1 = $ROWS;" "$EXPECTED"

{
    echo "id\tamount"
    awk -F '\t' 'NR > 1 { print $1 "\t" $3 }' "$BIG_TSV" | sort -n -k2,2 -k1,1 | head -5
} >"$EXPECTED"
run_timed_sql ordered-limit "SELECT c1 AS id, c3 AS amount FROM big ORDER BY c3 ASC, c1 ASC LIMIT 5;" "$EXPECTED"

echo "ok - stress SELECT checks passed"

if [ "$KEEP" = 0 ]; then
    rm -rf "$TMP_DIR"
else
    echo "kept generated files in $TMP_DIR"
fi