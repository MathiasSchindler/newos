#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir split_csplit)

note "phase1 text: split/csplit"

cat > "$WORK_DIR/split.txt" <<'EOF'
one
two
three
four
EOF

"${TEST_BIN_DIR}/split" -l 2 "$WORK_DIR/split.txt" "$WORK_DIR/part-"
printf 'one\ntwo\n' > "$WORK_DIR/part-aa.expected"
printf 'three\nfour\n' > "$WORK_DIR/part-ab.expected"
assert_files_equal "$WORK_DIR/part-aa.expected" "$WORK_DIR/part-aa" "split did not create the first chunk correctly"
assert_files_equal "$WORK_DIR/part-ab.expected" "$WORK_DIR/part-ab" "split did not create the second chunk correctly"

for short_name in -l -b -C -n -a; do
    case "$short_name" in
        -l) long_name=--lines ;;
        -b) long_name=--bytes ;;
        -C) long_name=--line-bytes ;;
        -n) long_name=--number ;;
        -a) long_name=--suffix-length ;;
    esac
    reference_prefix="$WORK_DIR/reference${short_name}-"
    "${TEST_BIN_DIR}/split" "$short_name" 2 "$WORK_DIR/split.txt" "$reference_prefix"
    set -- "$reference_prefix"*
    reference_count=$#
    for attached_arg in "${short_name}2" "${long_name}=2"; do
        attached_prefix="$WORK_DIR/attached${attached_arg}-"
        "${TEST_BIN_DIR}/split" "$attached_arg" "$WORK_DIR/split.txt" "$attached_prefix"
        set -- "$attached_prefix"*
        assert_text_equals "$#" "$reference_count" "split attached value changed the output file count"
        for reference_file in "$reference_prefix"*; do
            suffix=${reference_file#"$reference_prefix"}
            assert_files_equal "$reference_file" "$attached_prefix$suffix" "split attached value changed output"
        done
    done
done
for invalid_lines in --lines= --linesmore=2; do
    if "${TEST_BIN_DIR}/split" "$invalid_lines" "$WORK_DIR/split.txt" "$WORK_DIR/invalid-" > "$WORK_DIR/split_invalid.out" 2> "$WORK_DIR/split_invalid.err"; then
        fail "split accepted an invalid attached line count"
    fi
    assert_file_contains "$WORK_DIR/split_invalid.err" 'Usage:' "split did not diagnose an invalid attached line count"
done

cat > "$WORK_DIR/csplit.txt" <<'EOF'
alpha
beta
gamma
EOF

"${TEST_BIN_DIR}/csplit" -f "$WORK_DIR/chunk" "$WORK_DIR/csplit.txt" '/beta/' > "$WORK_DIR/csplit.out"
printf 'alpha\n' > "$WORK_DIR/chunk00.expected"
printf 'beta\ngamma\n' > "$WORK_DIR/chunk01.expected"
assert_files_equal "$WORK_DIR/chunk00.expected" "$WORK_DIR/chunk00" "csplit did not create the leading section correctly"
assert_files_equal "$WORK_DIR/chunk01.expected" "$WORK_DIR/chunk01" "csplit did not create the trailing section correctly"

printf 'abcdef\n' > "$WORK_DIR/split_bytes.txt"
(
    cd "$WORK_DIR"
    "${TEST_BIN_DIR}/split" -b 2 -a 3 -d split_bytes.txt part
)
assert_file_contains "$WORK_DIR/part000" '^ab$' "split -b/-d did not create the first numeric chunk"
assert_file_contains "$WORK_DIR/part001" '^cd$' "split -b/-d did not create the second numeric chunk"
assert_file_contains "$WORK_DIR/part002" '^ef$' "split -b/-d did not create the third numeric chunk"

if "${TEST_BIN_DIR}/split" -b 18446744073709551615g "$WORK_DIR/split.txt" "$WORK_DIR/overflow-" > /dev/null 2> "$WORK_DIR/split_overflow.err"
then
    fail "split accepted a byte size that overflowed its internal arithmetic"
fi
assert_file_contains "$WORK_DIR/split_overflow.err" '^Usage:' "split did not reject an overflowing -b size"

if "${TEST_BIN_DIR}/split" -a 18446744073709551615 "$WORK_DIR/split.txt" "$WORK_DIR/overflow-" > /dev/null 2> "$WORK_DIR/split_suffix.err"
then
    fail "split accepted an absurdly large suffix length"
fi
assert_file_contains "$WORK_DIR/split_suffix.err" 'split: too many output files for prefix ' "split did not fail safely on a huge suffix length"

if "${TEST_BIN_DIR}/csplit" -f "$WORK_DIR/repeat" "$WORK_DIR/csplit.txt" '/beta/{184467440737095516151}' > /dev/null 2> "$WORK_DIR/csplit_repeat.err"
then
    fail "csplit accepted a repeat count that overflowed"
fi
assert_file_contains "$WORK_DIR/csplit_repeat.err" 'csplit: invalid pattern ' "csplit did not reject an overflowing repeat count"
