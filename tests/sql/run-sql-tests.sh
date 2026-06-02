#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
SQL_BIN="$ROOT/build/sql"
CASE_DIR="$ROOT/tests/sql/cases"
TMP_DIR="$ROOT/tests/tmp"

mkdir -p "$TMP_DIR"

cd "$ROOT"
make host >/dev/null

failures=0
count=0

run_case() {
    case_file=$1
    name=$(basename "$case_file" .sql)
    expected="$CASE_DIR/$name.out"
    actual="$TMP_DIR/$name.actual"
    err="$TMP_DIR/$name.err"
    db="$TMP_DIR/$name.db"

    rm -f "$actual" "$err" "$db"
    : >"$actual"
    : >"$err"

    while IFS= read -r statement || [ -n "$statement" ]; do
        case "$statement" in
            ''|'#'*)
                continue
                ;;
        esac
        if ! "$SQL_BIN" "$db" "$statement" >>"$actual" 2>>"$err"; then
            echo "not ok - $name" >&2
            echo "statement failed: $statement" >&2
            if [ -s "$err" ]; then
                cat "$err" >&2
            fi
            failures=$((failures + 1))
            return
        fi
    done <"$case_file"

    if ! diff -u "$expected" "$actual"; then
        echo "not ok - $name" >&2
        if [ -s "$err" ]; then
            echo "stderr:" >&2
            cat "$err" >&2
        fi
        failures=$((failures + 1))
        return
    fi
    if [ -s "$err" ]; then
        echo "not ok - $name (unexpected stderr)" >&2
        cat "$err" >&2
        failures=$((failures + 1))
        return
    fi
    echo "ok - $name"
}

run_scale_case() {
    name="06-generated-scale"
    rows=12000
    cols=48
    db="$TMP_DIR/$name.db"
    tsv="$TMP_DIR/$name.tsv"
    actual="$TMP_DIR/$name.actual"
    expected="$TMP_DIR/$name.expected"
    err="$TMP_DIR/$name.err"
    create="CREATE TABLE big("
    col=1

    rm -f "$db" "$tsv" "$actual" "$expected" "$err"
    : >"$actual"
    : >"$err"

    while [ "$col" -le "$cols" ]; do
        if [ "$col" -gt 1 ]; then
            create="$create, "
        fi
        create="$create""c$col"
        col=$((col + 1))
    done
    create="$create);"

    awk -v rows="$rows" -v cols="$cols" 'BEGIN {
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
                printf "%d", r + c - 1
            }
            printf "\n"
        }
    }' >"$tsv"

    {
        echo "ok"
        echo "$rows rows"
        echo "count\tmax\tmin"
        echo "$rows\t$((rows + cols - 1))\t1"
        echo "c1\tc48"
        echo "$rows\t$((rows + cols - 1))"
    } >"$expected"

    if ! "$SQL_BIN" "$db" "$create IMPORT big FROM '$tsv'; SELECT COUNT(*), MAX(c48), MIN(c1) FROM big; SELECT c1, c48 FROM big WHERE c1 = $rows;" >>"$actual" 2>>"$err"; then
        echo "not ok - $name" >&2
        cat "$err" >&2
        failures=$((failures + 1))
        return
    fi
    if ! diff -u "$expected" "$actual"; then
        echo "not ok - $name" >&2
        if [ -s "$err" ]; then
            cat "$err" >&2
        fi
        failures=$((failures + 1))
        return
    fi
    if [ -s "$err" ]; then
        echo "not ok - $name (unexpected stderr)" >&2
        cat "$err" >&2
        failures=$((failures + 1))
        return
    fi
    echo "ok - $name"
}

expect_sql_failure() {
    name=$1
    sql=$2
    db="$TMP_DIR/$name.db"
    actual="$TMP_DIR/$name.actual"
    err="$TMP_DIR/$name.err"

    rm -f "$actual" "$err" "$db"
    : >"$actual"
    : >"$err"

    if "$SQL_BIN" "$db" "$sql" >"$actual" 2>"$err"; then
        echo "not ok - $name (unexpected success)" >&2
        cat "$actual" >&2
        failures=$((failures + 1))
        return
    fi
    if ! grep -q "invalid or unsupported SQL statement" "$err"; then
        echo "not ok - $name (unexpected stderr)" >&2
        cat "$err" >&2
        failures=$((failures + 1))
        return
    fi
    echo "ok - $name"
}

run_negative_cases() {
    expect_sql_failure "09-primary-key-duplicate" "CREATE TABLE users(id PRIMARY KEY, email UNIQUE); INSERT INTO users VALUES (1, a); INSERT INTO users VALUES (1, b);"
    expect_sql_failure "09-unique-duplicate" "CREATE TABLE users(id PRIMARY KEY, email UNIQUE); INSERT INTO users VALUES (1, a), (2, a);"
    expect_sql_failure "09-update-unique-duplicate" "CREATE TABLE users(id PRIMARY KEY, email UNIQUE); INSERT INTO users VALUES (1, a), (2, b); UPDATE users SET email = a WHERE id = 2;"
    expect_sql_failure "09-multiple-primary-keys" "CREATE TABLE bad(a PRIMARY KEY, b PRIMARY KEY);"
    expect_sql_failure "09-drop-last-column" "CREATE TABLE only(id); ALTER TABLE only DROP COLUMN id;"
    expect_sql_failure "09-add-unique-with-duplicate-defaults" "CREATE TABLE users(id); INSERT INTO users VALUES (1), (2); ALTER TABLE users ADD COLUMN tag UNIQUE DEFAULT same;"
    expect_sql_failure "10-invalid-integer-value" "CREATE TABLE typed(id INTEGER); INSERT INTO typed VALUES (1.25);"
    expect_sql_failure "10-invalid-real-value" "CREATE TABLE typed(amount REAL); INSERT INTO typed VALUES (not_number);"
    expect_sql_failure "10-natural-join-no-common-column" "CREATE TABLE a(id); CREATE TABLE b(code); SELECT * FROM a NATURAL JOIN b;"
}

for case_file in "$CASE_DIR"/*.sql; do
    count=$((count + 1))
    run_case "$case_file"
done

count=$((count + 1))
run_scale_case

count=$((count + 1))
run_negative_cases

if [ "$failures" -ne 0 ]; then
    echo "$failures of $count SQL test cases failed" >&2
    exit 1
fi

echo "all $count SQL test cases passed"
