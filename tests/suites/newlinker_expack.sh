#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BUILD_DIR=${NEWOS_NEWLINKER_BUILD_DIR:-$ROOT_DIR/build/freestanding-linux-$(uname -m)}
EXPACK=${NEWOS_EXPACK:-$ROOT_DIR/build/host-linux-x86_64/expack}
OUTPUT_DIR=${NEWOS_NEWLINKER_EXPACK_OUT:-$ROOT_DIR/build/freestanding-linux-expack}
EXPACK_FLAGS=${NEWOS_EXPACK_FLAGS---all}
JOBS=${PARALLEL_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
read -r -a EXPACK_FLAGS_ARRAY <<< "$EXPACK_FLAGS"
if [[ -n "${NEWOS_NEWLINKER_EXPACK_TOOLS:-}" ]]; then
    TOOLS=$NEWOS_NEWLINKER_EXPACK_TOOLS
else
    TOOLS=$(awk '/^TOOLS[[:space:]]*:=/{sub(/^[^:]*:=/,""); print; exit}' "$ROOT_DIR/Makefile")
fi

if ! [[ "$JOBS" =~ ^[0-9]+$ ]] || [[ "$JOBS" -lt 1 ]]; then
    JOBS=1
fi

if [[ ! -x "$EXPACK" ]]; then
    echo "missing expack: $EXPACK" >&2
    exit 1
fi
if [[ ! -d "$BUILD_DIR" ]]; then
    echo "missing newlinker build directory: $BUILD_DIR" >&2
    echo "build it first with make freestanding" >&2
    exit 1
fi

rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR/.logs" "$OUTPUT_DIR/.results"

pack_one() {
    local tool=$1
    local input=$BUILD_DIR/$tool
    local output=$OUTPUT_DIR/$tool
    local log=$OUTPUT_DIR/.logs/${tool//\//_}.log
    local result=$OUTPUT_DIR/.results/${tool//\//_}.tsv
    local input_size output_size

    [[ "$tool" == "[" ]] && log=$OUTPUT_DIR/.logs/lbracket.log && result=$OUTPUT_DIR/.results/lbracket.tsv
    if [[ ! -x "$input" ]]; then
        printf 'fail\t%s\tmissing newlinker tool: %s\n' "$tool" "$input" > "$result"
        return 1
    fi
    if ! "$EXPACK" -q "${EXPACK_FLAGS_ARRAY[@]}" "$input" "$output" >"$log" 2>&1; then
        printf 'fail\t%s\t' "$tool" > "$result"
        sed -n '1p' "$log" >> "$result"
        return 1
    fi
    if [[ ! -x "$output" ]]; then
        printf 'fail\t%s\texpack did not produce executable output\n' "$tool" > "$result"
        return 1
    fi
    input_size=$(wc -c < "$input")
    output_size=$(wc -c < "$output")
    printf 'ok\t%s\t%s\t%s\n' "$tool" "$input_size" "$output_size" > "$result"
}

status=0
for tool in $TOOLS; do
    pack_one "$tool" &
    while [[ $(jobs -rp | wc -l) -ge $JOBS ]]; do
        wait -n || status=1
    done
done
while [[ $(jobs -rp | wc -l) -gt 0 ]]; do
    wait -n || status=1
done

if grep -h '^fail' "$OUTPUT_DIR"/.results/*.tsv >/dev/null 2>&1; then
    status=1
    grep -h '^fail' "$OUTPUT_DIR"/.results/*.tsv >&2 || true
fi

if [[ "$status" -ne 0 ]]; then
    exit "$status"
fi

printf 'tool\tinput\tpacked\n'
for tool in $TOOLS; do
    result=$OUTPUT_DIR/.results/${tool//\//_}.tsv
    [[ "$tool" == "[" ]] && result=$OUTPUT_DIR/.results/lbracket.tsv
    awk -F '\t' '$1 == "ok" {printf "%s\t%s\t%s\n", $2, $3, $4}' "$result"
done

"$OUTPUT_DIR/true"
true_status=$?
if [[ "$true_status" -ne 0 ]]; then
    echo "packed true exited $true_status" >&2
    exit 1
fi

set +e
"$OUTPUT_DIR/false"
false_status=$?
set -e
if [[ "$false_status" -ne 1 ]]; then
    echo "packed false exited $false_status" >&2
    exit 1
fi

printf 'abc\n' > "$OUTPUT_DIR/.cat.in"
"$OUTPUT_DIR/cat" < "$OUTPUT_DIR/.cat.in" > "$OUTPUT_DIR/.cat.out"
if ! cmp -s "$OUTPUT_DIR/.cat.in" "$OUTPUT_DIR/.cat.out"; then
    echo "packed cat output mismatch" >&2
    exit 1
fi

pwd_output=$("$OUTPUT_DIR/pwd")
expected_pwd=$(pwd)
if [[ "$pwd_output" != "$expected_pwd" ]]; then
    echo "packed pwd output mismatch: got '$pwd_output', expected '$expected_pwd'" >&2
    exit 1
fi

count=$(printf '%s\n' $TOOLS | wc -l)
echo "NEWLINKER_EXPACK_OK tools=$count jobs=$JOBS flags=${EXPACK_FLAGS:-none} output=$OUTPUT_DIR"
