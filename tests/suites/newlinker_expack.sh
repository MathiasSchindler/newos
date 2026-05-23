#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BUILD_DIR=${NEWOS_NEWLINKER_BUILD_DIR:-$ROOT_DIR/build/freestanding-linux-newlinker}
EXPACK=${NEWOS_EXPACK:-$ROOT_DIR/build/host-linux-x86_64/expack}
TOOLS=${NEWOS_NEWLINKER_EXPACK_TOOLS:-true false cat linker ncc}
TMP_DIR=${NEWOS_NEWLINKER_EXPACK_TMP:-$ROOT_DIR/tests/tmp/newlinker-expack}

if [ ! -x "$EXPACK" ]; then
    echo "missing expack: $EXPACK" >&2
    exit 1
fi
if [ ! -d "$BUILD_DIR" ]; then
    echo "missing newlinker build directory: $BUILD_DIR" >&2
    echo "build it first, for example with build/build-freestanding-newlinker.sh" >&2
    exit 1
fi

rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR"

for tool in $TOOLS; do
    input=$BUILD_DIR/$tool
    output=$TMP_DIR/$tool-pack
    log=$TMP_DIR/$tool.log

    if [ ! -x "$input" ]; then
        echo "missing newlinker tool: $input" >&2
        exit 1
    fi
    "$EXPACK" -q "$input" "$output" >"$log" 2>&1
    if [ ! -x "$output" ]; then
        echo "expack did not produce executable output for $tool" >&2
        cat "$log" >&2
        exit 1
    fi
    input_size=$(wc -c < "$input")
    output_size=$(wc -c < "$output")
    printf '%s\t%s\t%s\n' "$tool" "$input_size" "$output_size"
done

"$TMP_DIR/true-pack"
true_status=$?
if [ "$true_status" -ne 0 ]; then
    echo "packed true exited $true_status" >&2
    exit 1
fi

set +e
"$TMP_DIR/false-pack"
false_status=$?
set -e
if [ "$false_status" -ne 1 ]; then
    echo "packed false exited $false_status" >&2
    exit 1
fi

printf 'abc\n' > "$TMP_DIR/cat.in"
"$TMP_DIR/cat-pack" < "$TMP_DIR/cat.in" > "$TMP_DIR/cat.out"
if ! cmp -s "$TMP_DIR/cat.in" "$TMP_DIR/cat.out"; then
    echo "packed cat output mismatch" >&2
    exit 1
fi

echo "NEWLINKER_EXPACK_OK"