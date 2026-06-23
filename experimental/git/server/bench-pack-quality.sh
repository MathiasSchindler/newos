#!/bin/sh
set -eu

PORT=${GITD_BENCH_PORT:-8140}
ROOT_DIR=$(cd ../../.. && pwd)
SERVER_DIR=$(pwd)
TMP_DIR="$ROOT_DIR/tests/tmp/gitd-pack-quality"
REPOS="$TMP_DIR/repos"
WORK="$TMP_DIR/work"
GITD=${GITD:-$SERVER_DIR/build/gitd}
SERVER_PID=

cleanup() {
    if [ -n "${SERVER_PID:-}" ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT HUP INT TERM

rm -rf "$TMP_DIR"
mkdir -p "$REPOS" "$WORK"

create_paired_repo() {
    repo_name=$1
    pair_count=$2
    line_count=$3
    repo_work="$WORK/$repo_name"

    mkdir -p "$repo_work"
    cd "$repo_work"
    git init -q
    git config user.name 'gitd pack bench'
    git config user.email gitd-pack-bench@example.invalid
    for pair in $(seq 1 "$pair_count"); do
        for variant in a b; do
            file=$(printf 'pair%03d-%s.txt' "$pair" "$variant")
            for line in $(seq 1 "$line_count"); do
                if [ "$variant" = b ] && [ "$line" = 80 ]; then
                    printf 'pair %03d changed line %03d\n' "$pair" "$line"
                else
                    printf 'pair %03d stable line %03d common payload common payload\n' "$pair" "$line"
                fi
            done > "$file"
        done
    done
    git add .
    git commit -q -m 'paired blobs'
    git branch -M main
    git clone --bare . "$REPOS/$repo_name.git" >/dev/null 2>&1
}

create_generated_repo() {
    repo_name=$1
    file_count=$2
    repo_work="$WORK/$repo_name"

    mkdir -p "$repo_work/src"
    cd "$repo_work"
    git init -q
    git config user.name 'gitd pack bench'
    git config user.email gitd-pack-bench@example.invalid
    for file_index in $(seq 1 "$file_count"); do
        path=$(printf 'src/generated_%03d.c' "$file_index")
        {
            printf '/* generated file %03d */\n' "$file_index"
            printf 'static int value_%03d(void) {\n' "$file_index"
            for line in $(seq 1 90); do
                printf '    int item_%03d_%03d = %d + %d;\n' "$file_index" "$line" "$file_index" "$line"
            done
            printf '    return item_%03d_090;\n' "$file_index"
            printf '}\n'
        } > "$path"
    done
    git add .
    git commit -q -m 'generated sources'
    git branch -M main
    git clone --bare . "$REPOS/$repo_name.git" >/dev/null 2>&1
}

create_mixed_repo() {
    repo_name=$1
    file_count=$2
    repo_work="$WORK/$repo_name"

    mkdir -p "$repo_work/data"
    cd "$repo_work"
    git init -q
    git config user.name 'gitd pack bench'
    git config user.email gitd-pack-bench@example.invalid
    for file_index in $(seq 1 "$file_count"); do
        path=$(printf 'data/mixed_%03d.txt' "$file_index")
        for line in $(seq 1 120); do
            value=$((file_index * 1103515245 + line * 12345))
            printf 'mixed %03d line %03d value %08x text %08x\n' "$file_index" "$line" "$value" "$((value ^ 305419896))"
        done > "$path"
    done
    git add .
    git commit -q -m 'mixed blobs'
    git branch -M main
    git clone --bare . "$REPOS/$repo_name.git" >/dev/null 2>&1
}

native_pack_size() {
    repo=$1
    out=$2

    git --git-dir="$repo" rev-list --objects --all | sed 's/ .*//' | git --git-dir="$repo" pack-objects --quiet --stdout > "$out" 2>"$out.log"
    wc -c < "$out" | tr -d ' '
}

gitd_clone_pack_size() {
    repo_url=$1
    clone_dir=$2

    rm -rf "$clone_dir"
    git -c transfer.unpackLimit=1 clone "$repo_url" "$clone_dir" >/dev/null 2>&1
    pack_idx=$(find "$clone_dir/.git/objects/pack" -name '*.idx' -print -quit)
    if [ -z "$pack_idx" ]; then
        echo 0
        return 1
    fi
    pack_file=${pack_idx%.idx}.pack
    wc -c < "$pack_file" | tr -d ' '
}

count_delta_entries() {
    clone_dir=$1
    verify_out=$2

    pack_idx=$(find "$clone_dir/.git/objects/pack" -name '*.idx' -print -quit)
    git verify-pack -v "$pack_idx" > "$verify_out"
    awk '$2 == "blob" && NF >= 7 {count++} END {print count + 0}' "$verify_out"
}

create_paired_repo paired80 80 160
create_generated_repo generated120 120
create_mixed_repo mixed80 80

cd "$ROOT_DIR"
"$GITD" -q -r "$REPOS" -p "$PORT" >"$TMP_DIR/gitd.out" 2>"$TMP_DIR/gitd.err" &
SERVER_PID=$!
ready=0
attempt=0
while [ "$attempt" -lt 500 ]; do
    if curl -fsS --connect-timeout 1 -o /dev/null "http://127.0.0.1:$PORT/health" 2>/dev/null; then
        ready=1
        break
    fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        cat "$TMP_DIR/gitd.err" >&2 || true
        echo 'gitd exited before becoming ready' >&2
        exit 1
    fi
    attempt=$((attempt + 1))
done
if [ "$ready" -ne 1 ]; then
    cat "$TMP_DIR/gitd.err" >&2 || true
    echo 'gitd did not become ready' >&2
    exit 1
fi

printf 'fixture\tgitd_pack\tgit_pack\tratio_pct\tdelta_entries\n'
for fixture in paired80 generated120 mixed80; do
    git_pack="$TMP_DIR/$fixture.git.pack"
    git_size=$(native_pack_size "$REPOS/$fixture.git" "$git_pack")
    clone_dir="$TMP_DIR/clone-$fixture"
    gitd_size=$(gitd_clone_pack_size "http://127.0.0.1:$PORT/$fixture.git" "$clone_dir")
    delta_entries=$(count_delta_entries "$clone_dir" "$TMP_DIR/$fixture.verify")
    ratio=$((gitd_size * 100 / git_size))
    printf '%s\t%s\t%s\t%s\t%s\n' "$fixture" "$gitd_size" "$git_size" "$ratio" "$delta_entries"
done

printf 'wrote %s\n' "$TMP_DIR" >&2
