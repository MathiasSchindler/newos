#!/bin/sh
set -eu

PORT=${GITD_TEST_PORT:-8092}
ROOT_DIR=$(cd ../../.. && pwd)
SERVER_DIR=$(pwd)
TMP_DIR="$ROOT_DIR/tests/tmp/gitd-server-smoke"
REPOS="$TMP_DIR/repos"
WORK="$TMP_DIR/work"
CLONE_NATIVE="$TMP_DIR/clone-native"
CLONE_NEWOS="$TMP_DIR/clone-newos"
PUSH_NATIVE="$TMP_DIR/push-native"
GITD="$SERVER_DIR/build/gitd"
NEWOS_GIT=${NEWOS_GIT:-$ROOT_DIR/build/macos-aarch64/git}

rm -rf "$TMP_DIR"
mkdir -p "$WORK" "$REPOS"

cd "$WORK"
git init -q
git config user.name 'newos gitd smoke'
git config user.email gitd@example.invalid
printf 'hello from gitd\n' > README.md
mkdir -p src
printf 'int main(void) { return 0; }\n' > src/main.c
git add README.md src/main.c
git commit -q -m initial
printf 'int main(void) { return 1; }\n' > src/main.c
git add src/main.c
git commit -q -m update-main
git branch -M main
git clone --bare . "$REPOS/example.git" >/dev/null 2>&1

mkdir -p "$TMP_DIR/filter-work"
cd "$TMP_DIR/filter-work"
git init -q
git config user.name 'newos gitd smoke'
git config user.email gitd@example.invalid
for i in $(seq 1 80); do
    printf 'filter payload %03d\n' "$i" > "blob-$i.txt"
done
git add .
git commit -q -m 'many blobs'
git branch -M main
git clone --bare . "$REPOS/filter.git" >/dev/null 2>&1

cd "$ROOT_DIR"
"$GITD" -q -r "$REPOS" -p "$PORT" 2>"$TMP_DIR/gitd.log" &
SERVER_PID=$!
trap 'kill "$SERVER_PID" >/dev/null 2>&1 || true; wait "$SERVER_PID" 2>/dev/null || true; rm -rf "$TMP_DIR"' EXIT HUP INT TERM

ready=0
attempt=0
while [ "$attempt" -lt 500 ]; do
    if curl -fsS --connect-timeout 1 -o /dev/null "http://127.0.0.1:$PORT/health" 2>/dev/null; then
        ready=1
        break
    fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        cat "$TMP_DIR/gitd.log" >&2 || true
        echo 'gitd exited before becoming ready' >&2
        exit 1
    fi
    attempt=$((attempt + 1))
done
if [ "$ready" -ne 1 ]; then
    cat "$TMP_DIR/gitd.log" >&2 || true
    echo 'gitd did not become ready' >&2
    exit 1
fi

curl -fsS -D "$TMP_DIR/options.headers" -o "$TMP_DIR/options.body" \
    -X OPTIONS \
    -H 'Origin: http://127.0.0.1:8080' \
    -H 'Access-Control-Request-Method: POST' \
    -H 'Access-Control-Request-Headers: content-type' \
    "http://127.0.0.1:$PORT/example.git/git-upload-pack"
grep -qi '^Access-Control-Allow-Origin: \*' "$TMP_DIR/options.headers"
grep -qi '^Access-Control-Allow-Methods: .*POST' "$TMP_DIR/options.headers"
test ! -s "$TMP_DIR/options.body"

curl -fsS -D "$TMP_DIR/refs.headers" -o "$TMP_DIR/refs.body" \
    -H 'Origin: http://127.0.0.1:8080' \
    "http://127.0.0.1:$PORT/example.git/info/refs?service=git-upload-pack"
grep -qi '^Content-Type: application/x-git-upload-pack-advertisement' "$TMP_DIR/refs.headers"
grep -qi '^Access-Control-Allow-Origin: \*' "$TMP_DIR/refs.headers"
grep -aq '# service=git-upload-pack' "$TMP_DIR/refs.body"

v2_status=$(curl -sS -o "$TMP_DIR/v2.body" -w '%{http_code}' \
    -H 'Git-Protocol: version=2' \
    "http://127.0.0.1:$PORT/example.git/info/refs?service=git-upload-pack")
test "$v2_status" = 200
grep -aq 'version 2' "$TMP_DIR/v2.body"
grep -aq 'fetch=shallow filter wait-for-done' "$TMP_DIR/v2.body"

append_pkt() {
    payload=$1
    length=$((4 + ${#payload}))
    printf '%04x%s' "$length" "$payload"
}

{
    append_pkt 'command=ls-refs
'
    append_pkt 'agent=gitd-smoke
'
    append_pkt 'object-format=sha1
'
    printf '0001'
    append_pkt 'peel
'
    append_pkt 'symrefs
'
    append_pkt 'ref-prefix refs/heads/
'
    append_pkt 'ref-prefix HEAD
'
    printf '0000'
} > "$TMP_DIR/v2-lsrefs.request"
v2_lsrefs_status=$(curl -sS -o "$TMP_DIR/v2-lsrefs.body" -w '%{http_code}' \
    -H 'Git-Protocol: version=2' \
    -H 'Content-Type: application/x-git-upload-pack-request' \
    --data-binary "@$TMP_DIR/v2-lsrefs.request" \
    "http://127.0.0.1:$PORT/example.git/git-upload-pack")
test "$v2_lsrefs_status" = 200
grep -aq 'refs/heads/main' "$TMP_DIR/v2-lsrefs.body"

curl -fsS -D "$TMP_DIR/receive-refs.headers" -o "$TMP_DIR/receive-refs.body" \
    -H 'Origin: http://127.0.0.1:8080' \
    "http://127.0.0.1:$PORT/example.git/info/refs?service=git-receive-pack"
grep -qi '^Content-Type: application/x-git-receive-pack-advertisement' "$TMP_DIR/receive-refs.headers"
grep -qi '^Access-Control-Allow-Origin: \*' "$TMP_DIR/receive-refs.headers"
grep -aq '# service=git-receive-pack' "$TMP_DIR/receive-refs.body"
grep -aq 'report-status' "$TMP_DIR/receive-refs.body"
grep -aq 'delete-refs' "$TMP_DIR/receive-refs.body"

head_oid=$(git --git-dir="$REPOS/example.git" rev-parse HEAD)
{
    append_pkt "want $head_oid side-band-64k\n"
    printf '0000'
} > "$TMP_DIR/multiround-upload.request"
multiround_status=$(curl -sS -o "$TMP_DIR/multiround-upload.body" -w '%{http_code}' \
    -H 'Content-Type: application/x-git-upload-pack-request' \
    --data-binary "@$TMP_DIR/multiround-upload.request" \
    "http://127.0.0.1:$PORT/example.git/git-upload-pack")
test "$multiround_status" = 200
grep -aq 'NAK' "$TMP_DIR/multiround-upload.body"

git -c transfer.unpackLimit=1 clone "http://127.0.0.1:$PORT/example.git" "$CLONE_NATIVE" >/dev/null 2>&1
test "$(cat "$CLONE_NATIVE/README.md")" = 'hello from gitd'
native_pack_idx=$(find "$CLONE_NATIVE/.git/objects/pack" -name '*.idx' -print -quit)
test -n "$native_pack_idx"
git verify-pack -v "$native_pack_idx" > "$TMP_DIR/native-verify-pack.out"
grep -q '^chain length = 1:' "$TMP_DIR/native-verify-pack.out"

git -c protocol.version=2 clone --depth=1 "http://127.0.0.1:$PORT/example.git" "$TMP_DIR/clone-shallow-v2" >/dev/null 2>&1
test -s "$TMP_DIR/clone-shallow-v2/.git/shallow"
test "$(cat "$TMP_DIR/clone-shallow-v2/README.md")" = 'hello from gitd'

git -c protocol.version=2 clone --filter=blob:none "http://127.0.0.1:$PORT/example.git" "$TMP_DIR/clone-filter-v2" >/dev/null 2>&1
test "$(cat "$TMP_DIR/clone-filter-v2/README.md")" = 'hello from gitd'

git -c protocol.version=2 clone --filter=blob:none "http://127.0.0.1:$PORT/filter.git" "$TMP_DIR/clone-filter-gzip-v2" >/dev/null 2>&1
test "$(cat "$TMP_DIR/clone-filter-gzip-v2/blob-80.txt")" = 'filter payload 080'

git clone "http://127.0.0.1:$PORT/example.git" "$PUSH_NATIVE" >/dev/null 2>&1
cd "$PUSH_NATIVE"
git config user.name 'newos gitd smoke'
git config user.email gitd@example.invalid
printf 'fast-forward push\n' >> README.md
git add README.md
git commit -q -m 'fast-forward push'
git push origin main >/dev/null 2>&1
git switch -c feature >/dev/null 2>&1
printf 'feature branch\n' > feature.txt
git add feature.txt
git commit -q -m 'feature branch'
git push origin feature >/dev/null 2>&1
git tag v1
git push origin v1 >/dev/null 2>&1
git update-ref refs/notes/review HEAD
git push origin refs/notes/review >/dev/null 2>&1
git update-ref refs/meta/check HEAD
git push origin refs/meta/check >/dev/null 2>&1
git --git-dir="$REPOS/example.git" show main:README.md > "$TMP_DIR/remote-readme-after-push"
grep -q 'fast-forward push' "$TMP_DIR/remote-readme-after-push"
git --git-dir="$REPOS/example.git" show-ref --verify refs/heads/feature >/dev/null
git --git-dir="$REPOS/example.git" show-ref --verify refs/tags/v1 >/dev/null
git --git-dir="$REPOS/example.git" show-ref --verify refs/notes/review >/dev/null
git --git-dir="$REPOS/example.git" show-ref --verify refs/meta/check >/dev/null

cd "$CLONE_NATIVE"
git config user.name 'newos gitd smoke'
git config user.email gitd@example.invalid
printf 'forced stale push\n' >> README.md
git add README.md
git commit -q -m 'stale push'
if git push --force origin main >"$TMP_DIR/stale-push.out" 2>"$TMP_DIR/stale-push.err"; then
    echo 'forced stale push unexpectedly succeeded' >&2
    exit 1
fi
git --git-dir="$REPOS/example.git" show main:README.md > "$TMP_DIR/remote-readme-after-reject"
grep -q 'fast-forward push' "$TMP_DIR/remote-readme-after-reject"
if grep -q 'forced stale push' "$TMP_DIR/remote-readme-after-reject"; then
    echo 'non-fast-forward push changed the remote ref' >&2
    exit 1
fi

cd "$PUSH_NATIVE"
git push origin :feature >/dev/null 2>&1
git push origin :v1 >/dev/null 2>&1
git push origin :refs/notes/review >/dev/null 2>&1
git push origin :refs/meta/check >/dev/null 2>&1
if git --git-dir="$REPOS/example.git" show-ref --verify refs/heads/feature >/dev/null 2>&1; then
    echo 'branch deletion did not remove remote ref' >&2
    exit 1
fi
if git --git-dir="$REPOS/example.git" show-ref --verify refs/tags/v1 >/dev/null 2>&1; then
    echo 'tag deletion did not remove remote ref' >&2
    exit 1
fi
if git --git-dir="$REPOS/example.git" show-ref --verify refs/notes/review >/dev/null 2>&1; then
    echo 'notes deletion did not remove remote ref' >&2
    exit 1
fi
if git --git-dir="$REPOS/example.git" show-ref --verify refs/meta/check >/dev/null 2>&1; then
    echo 'custom ref deletion did not remove remote ref' >&2
    exit 1
fi

cd "$ROOT_DIR"

if [ -x "$NEWOS_GIT" ]; then
    "$NEWOS_GIT" clone "http://127.0.0.1:$PORT/example.git" "$CLONE_NEWOS" >/dev/null 2>&1
    grep -q 'hello from gitd' "$CLONE_NEWOS/README.md"
    grep -q 'fast-forward push' "$CLONE_NEWOS/README.md"
fi

echo 'gitd smoke: ok'