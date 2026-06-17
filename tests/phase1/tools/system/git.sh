#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup git

repo="$WORK_DIR/repo"
mkdir -p "$repo/.git/refs/heads"
printf 'ref: refs/heads/main\n' > "$repo/.git/HEAD"
printf '0123456789abcdef0123456789abcdef01234567\n' > "$repo/.git/refs/heads/main"
printf '*.tmp\nignored-dir/\n' > "$repo/.gitignore"
mkdir -p "$repo/.git/info"
printf 'excluded.log\n' > "$repo/.git/info/exclude"
printf 'hello\n' > "$repo/tracked.txt"
printf 'changed\n' > "$repo/modified.txt"
printf 'gone\n' > "$repo/deleted.txt"
printf '#!/bin/sh\n' > "$repo/script.sh"
chmod 755 "$repo/script.sh"
printf 'loose\n' > "$repo/untracked.txt"
printf 'ignored\n' > "$repo/skip.tmp"
printf 'ignored\n' > "$repo/excluded.log"
mkdir -p "$repo/ignored-dir"
printf 'ignored\n' > "$repo/ignored-dir/file.txt"

tracked_oid=$(cd "$repo" && "${TEST_BIN_DIR}/git" hash-object tracked.txt | tr -d '\r\n')
modified_oid=$(cd "$repo" && "${TEST_BIN_DIR}/git" hash-object modified.txt | tr -d '\r\n')
deleted_oid=$(cd "$repo" && "${TEST_BIN_DIR}/git" hash-object deleted.txt | tr -d '\r\n')
script_oid=$(cd "$repo" && "${TEST_BIN_DIR}/git" hash-object script.sh | tr -d '\r\n')
printf 'original\n' > "$repo/modified.txt"
rm -f "$repo/deleted.txt"
chmod 644 "$repo/script.sh"

make_index_entry() {
    mode=$1
    size=$2
    oid=$3
    path=$4
    python3 - "$mode" "$size" "$oid" "$path" <<'PY'
import binascii
import struct
import sys

mode = int(sys.argv[1], 8)
size = int(sys.argv[2])
oid = binascii.unhexlify(sys.argv[3])
path = sys.argv[4].encode()
flags = len(path)
entry = struct.pack('!LLLLLLLLLL20sH', 0, 0, 0, 0, 0, 0, mode, 0, 0, size, oid, flags) + path + b'\0'
entry += b'\0' * ((8 - (len(entry) % 8)) % 8)
sys.stdout.buffer.write(entry)
PY
}

{
    printf 'DIRC'
    python3 - <<'PY'
import struct
import sys
sys.stdout.buffer.write(struct.pack('!LL', 2, 4))
PY
    make_index_entry 100644 8 "$deleted_oid" deleted.txt
    make_index_entry 100644 7 "$modified_oid" modified.txt
    make_index_entry 100755 10 "$script_oid" script.sh
    make_index_entry 100644 6 "$tracked_oid" tracked.txt
} > "$repo/.git/index"

branch=$(cd "$repo" && "${TEST_BIN_DIR}/git" branch --show-current | tr -d '\r\n')
assert_text_equals "$branch" 'main' "git branch --show-current did not print the current branch"

top=$(cd "$repo/subdir" 2>/dev/null && pwd || true)
if [ -z "$top" ]; then
    mkdir -p "$repo/subdir"
fi
show_top=$(cd "$repo/subdir" && "${TEST_BIN_DIR}/git" rev-parse --show-toplevel | tr -d '\r\n')
assert_text_equals "$show_top" "$repo" "git rev-parse --show-toplevel did not find the repository root"

head_oid=$(cd "$repo" && "${TEST_BIN_DIR}/git" rev-parse HEAD | tr -d '\r\n')
assert_text_equals "$head_oid" '0123456789abcdef0123456789abcdef01234567' "git rev-parse HEAD did not resolve a branch ref"

cd "$repo" && "${TEST_BIN_DIR}/git" ls-files > "$WORK_DIR/ls-files.out"
assert_file_contains "$WORK_DIR/ls-files.out" '^deleted.txt$' "git ls-files missed deleted.txt"
assert_file_contains "$WORK_DIR/ls-files.out" '^modified.txt$' "git ls-files missed modified.txt"
assert_file_contains "$WORK_DIR/ls-files.out" '^tracked.txt$' "git ls-files missed tracked.txt"

cd "$repo" && "${TEST_BIN_DIR}/git" status --short > "$WORK_DIR/status.out"
assert_file_contains "$WORK_DIR/status.out" '^ D deleted.txt$' "git status did not report a deleted tracked file"
assert_file_contains "$WORK_DIR/status.out" '^ M modified.txt$' "git status did not report a modified tracked file"
assert_file_contains "$WORK_DIR/status.out" '^ M script.sh$' "git status did not report an executable-bit change"
assert_file_contains "$WORK_DIR/status.out" '^?? untracked.txt$' "git status did not report an untracked file"
if grep -q '^.. tracked\.txt$' "$WORK_DIR/status.out"; then
    fail "git status reported an unchanged tracked file"
fi
if grep -q 'skip.tmp\|excluded.log\|ignored-dir' "$WORK_DIR/status.out"; then
    fail "git status reported ignored files"
fi

hash_again=$(cd "$repo" && "${TEST_BIN_DIR}/git" hash-object tracked.txt | tr -d '\r\n')
assert_text_equals "$hash_again" "$tracked_oid" "git hash-object was not stable"

clean="$WORK_DIR/clean-source"
clone="$WORK_DIR/cloned-copy"
mkdir -p "$clean/.git/refs/heads" "$clean/dir"
printf 'ref: refs/heads/main\n' > "$clean/.git/HEAD"
printf 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n' > "$clean/.git/refs/heads/main"
printf 'root\n' > "$clean/root.txt"
printf 'nested\n' > "$clean/dir/nested.txt"
printf '#!/bin/sh\n' > "$clean/run.sh"
chmod 755 "$clean/run.sh"
printf 'loose\n' > "$clean/untracked.txt"

clean_root_oid=$(cd "$clean" && "${TEST_BIN_DIR}/git" hash-object root.txt | tr -d '\r\n')
clean_nested_oid=$(cd "$clean" && "${TEST_BIN_DIR}/git" hash-object dir/nested.txt | tr -d '\r\n')
clean_run_oid=$(cd "$clean" && "${TEST_BIN_DIR}/git" hash-object run.sh | tr -d '\r\n')
{
    printf 'DIRC'
    python3 - <<'PY'
import struct
import sys
sys.stdout.buffer.write(struct.pack('!LL', 2, 3))
PY
    make_index_entry 100644 7 "$clean_nested_oid" dir/nested.txt
    make_index_entry 100644 5 "$clean_root_oid" root.txt
    make_index_entry 100755 10 "$clean_run_oid" run.sh
} > "$clean/.git/index"

cd "$WORK_DIR" && "${TEST_BIN_DIR}/git" clone "$clean" "$clone" > "$WORK_DIR/clone.out"
assert_file_contains "$WORK_DIR/clone.out" 'Cloned local repository' "git clone did not report a local clone"
assert_files_equal "$clean/root.txt" "$clone/root.txt" "git clone did not copy a root tracked file"
assert_files_equal "$clean/dir/nested.txt" "$clone/dir/nested.txt" "git clone did not copy a nested tracked file"
if [ ! -x "$clone/run.sh" ]; then
    fail "git clone did not preserve an executable tracked file"
fi
if [ -e "$clone/untracked.txt" ]; then
    fail "git clone copied an untracked source file"
fi
clone_branch=$(cd "$clone" && "${TEST_BIN_DIR}/git" branch --show-current | tr -d '\r\n')
assert_text_equals "$clone_branch" 'main' "git clone did not copy repository metadata"
cd "$clone" && "${TEST_BIN_DIR}/git" status --short > "$WORK_DIR/clone-status.out"
assert_text_equals "$(cat "$WORK_DIR/clone-status.out")" '' "git clone destination should be clean"

checkout_repo="$WORK_DIR/checkout-repo"
mkdir -p "$checkout_repo/.git/refs/heads" "$checkout_repo/.git/objects"
checkout_commit=$(python3 - "$checkout_repo" <<'PY'
import hashlib
import os
import sys
import zlib

repo = sys.argv[1]

def write_object(kind, payload):
    full = kind.encode() + b" " + str(len(payload)).encode() + b"\0" + payload
    oid = hashlib.sha1(full).hexdigest()
    directory = os.path.join(repo, ".git", "objects", oid[:2])
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, oid[2:]), "wb") as handle:
        handle.write(zlib.compress(full))
    return oid

blob = write_object("blob", b"checkout\n")
link_blob = write_object("blob", b"file.txt")
tree = write_object("tree", b"100644 file.txt\0" + bytes.fromhex(blob) + b"120000 link.txt\0" + bytes.fromhex(link_blob))
commit = write_object("commit", ("tree " + tree + "\n\ninitial\n").encode())
print(commit)
PY
)
printf 'ref: refs/heads/main\n' > "$checkout_repo/.git/HEAD"
printf '%s\n' "$checkout_commit" > "$checkout_repo/.git/refs/heads/main"
cd "$checkout_repo" && "${TEST_BIN_DIR}/git" checkout main > "$WORK_DIR/checkout.out"
assert_file_contains "$WORK_DIR/checkout.out" 'Checked out' "git checkout did not report success"
assert_file_contains "$checkout_repo/file.txt" '^checkout$' "git checkout did not write a blob from the tree"
if [ ! -L "$checkout_repo/link.txt" ]; then
    fail "git checkout did not write a symlink from the tree"
fi
link_target=$(readlink "$checkout_repo/link.txt")
assert_text_equals "$link_target" 'file.txt' "git checkout wrote the wrong symlink target"
cd "$checkout_repo" && "${TEST_BIN_DIR}/git" status --short > "$WORK_DIR/checkout-status.out"
assert_text_equals "$(cat "$WORK_DIR/checkout-status.out")" '' "git checkout destination should be clean"