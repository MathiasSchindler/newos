#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup git

init_repo="$WORK_DIR/init-repo"
"${TEST_BIN_DIR}/git" init "$init_repo" > "$WORK_DIR/init.out"
assert_file_contains "$WORK_DIR/init.out" 'Initialized empty Git repository' "git init did not report initialization"
if [ ! -d "$init_repo/.git/refs/tags" ]; then
    fail "git init did not create refs/tags"
fi
inside=$("${TEST_BIN_DIR}/git" -C "$init_repo" rev-parse --is-inside-work-tree | tr -d '\r\n')
assert_text_equals "$inside" 'true' "git -C rev-parse --is-inside-work-tree failed"
init_branch=$("${TEST_BIN_DIR}/git" -C "$init_repo" branch --show-current | tr -d '\r\n')
assert_text_equals "$init_branch" 'main' "git init did not write a main HEAD"
"${TEST_BIN_DIR}/git" -C "$init_repo" config user.name Alice
"${TEST_BIN_DIR}/git" -C "$init_repo" config user.email alice@example.invalid
config_name=$("${TEST_BIN_DIR}/git" -C "$init_repo" config user.name | tr -d '\r\n')
assert_text_equals "$config_name" 'Alice' "git config did not read back user.name"
config_email=$("${TEST_BIN_DIR}/git" -C "$init_repo" config user.email | tr -d '\r\n')
assert_text_equals "$config_email" 'alice@example.invalid' "git config did not read back user.email"
if "${TEST_BIN_DIR}/git" -C "$init_repo" config color.ui > "$WORK_DIR/config-missing.out" 2>/dev/null; then
    fail "git config returned success for an unsupported or missing key"
fi
"${TEST_BIN_DIR}/git" -C "$init_repo" remote add origin https://example.invalid/repo.git
"${TEST_BIN_DIR}/git" -C "$init_repo" remote > "$WORK_DIR/remote-list.out"
assert_file_contains "$WORK_DIR/remote-list.out" '^origin$' "git remote did not list the added remote"
"${TEST_BIN_DIR}/git" -C "$init_repo" remote -v > "$WORK_DIR/remote-v.out"
assert_file_contains "$WORK_DIR/remote-v.out" '^origin[[:space:]]*https://example\.invalid/repo\.git (fetch)$' "git remote -v did not list the added fetch URL"
remote_fetch=$("${TEST_BIN_DIR}/git" -C "$init_repo" config remote.origin.fetch | tr -d '\r\n')
assert_text_equals "$remote_fetch" '+refs/heads/*:refs/remotes/origin/*' "git remote add did not write the fetch refspec"
"${TEST_BIN_DIR}/git" -C "$init_repo" remote set-url origin https://example.invalid/other.git
remote_url=$("${TEST_BIN_DIR}/git" -C "$init_repo" config remote.origin.url | tr -d '\r\n')
assert_text_equals "$remote_url" 'https://example.invalid/other.git' "git remote set-url did not update remote.origin.url"

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
mkdir -p "$repo/untracked-dir"
printf 'nested\n' > "$repo/untracked-dir/nested.txt"
printf 'ignored\n' > "$repo/skip.tmp"
printf 'ignored\n' > "$repo/excluded.log"
mkdir -p "$repo/ignored-dir"
printf 'ignored\n' > "$repo/ignored-dir/file.txt"

tracked_oid=$(cd "$repo" && "${TEST_BIN_DIR}/git" hash-object tracked.txt | tr -d '\r\n')
modified_oid=$(cd "$repo" && "${TEST_BIN_DIR}/git" hash-object modified.txt | tr -d '\r\n')
deleted_oid=$(cd "$repo" && "${TEST_BIN_DIR}/git" hash-object deleted.txt | tr -d '\r\n')
script_oid=$(cd "$repo" && "${TEST_BIN_DIR}/git" hash-object script.sh | tr -d '\r\n')

write_loose_blob() {
    repo_dir=$1
    path=$2
    python3 - "$repo_dir" "$path" <<'PY'
import hashlib
import os
import sys
import zlib

repo = sys.argv[1]
path = sys.argv[2]
with open(os.path.join(repo, path), "rb") as handle:
    payload = handle.read()
full = b"blob " + str(len(payload)).encode() + b"\0" + payload
oid = hashlib.sha1(full).hexdigest()
directory = os.path.join(repo, ".git", "objects", oid[:2])
os.makedirs(directory, exist_ok=True)
with open(os.path.join(directory, oid[2:]), "wb") as handle:
    handle.write(zlib.compress(full))
PY
}

write_loose_blob "$repo" tracked.txt
write_loose_blob "$repo" modified.txt
write_loose_blob "$repo" deleted.txt
write_loose_blob "$repo" script.sh
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

cd "$repo" && "${TEST_BIN_DIR}/git" ls-files -z --modified --deleted --stage > "$WORK_DIR/ls-files-machine.out"
tr '\000' '\n' < "$WORK_DIR/ls-files-machine.out" > "$WORK_DIR/ls-files-machine.lines"
assert_file_contains "$WORK_DIR/ls-files-machine.lines" '^100644 [0-9a-f][0-9a-f].* 0[[:space:]]deleted.txt$' "git ls-files --deleted --stage missed deleted.txt"
assert_file_contains "$WORK_DIR/ls-files-machine.lines" '^100644 [0-9a-f][0-9a-f].* 0[[:space:]]modified.txt$' "git ls-files --modified --stage missed modified.txt"
assert_file_contains "$WORK_DIR/ls-files-machine.lines" '^100755 [0-9a-f][0-9a-f].* 0[[:space:]]script.sh$' "git ls-files --modified --stage missed an executable-bit change"
if grep -q '^100644 .* 0[[:space:]]tracked\.txt$' "$WORK_DIR/ls-files-machine.lines"; then
    fail "git ls-files --modified --deleted reported an unchanged tracked path"
fi

cd "$repo" && "${TEST_BIN_DIR}/git" ls-files --cached -- modified.txt tracked.txt > "$WORK_DIR/ls-files-cached-pathspec.out"
assert_file_contains "$WORK_DIR/ls-files-cached-pathspec.out" '^modified.txt$' "git ls-files --cached pathspec missed modified.txt"
assert_file_contains "$WORK_DIR/ls-files-cached-pathspec.out" '^tracked.txt$' "git ls-files --cached pathspec missed tracked.txt"
if grep -q '^deleted\.txt$' "$WORK_DIR/ls-files-cached-pathspec.out"; then
    fail "git ls-files --cached pathspec reported an unrequested tracked path"
fi

cd "$repo" && "${TEST_BIN_DIR}/git" ls-files --others --exclude-standard > "$WORK_DIR/ls-files-others.out"
assert_file_contains "$WORK_DIR/ls-files-others.out" '^untracked.txt$' "git ls-files --others missed an untracked file"
assert_file_contains "$WORK_DIR/ls-files-others.out" '^untracked-dir/nested.txt$' "git ls-files --others missed an untracked nested file"
if grep -q 'skip.tmp\|excluded.log\|ignored-dir' "$WORK_DIR/ls-files-others.out"; then
    fail "git ls-files --others --exclude-standard reported ignored files"
fi

cd "$repo" && "${TEST_BIN_DIR}/git" ls-files --others --exclude-standard -- untracked-dir > "$WORK_DIR/ls-files-others-pathspec.out"
assert_file_contains "$WORK_DIR/ls-files-others-pathspec.out" '^untracked-dir/nested.txt$' "git ls-files --others pathspec missed a nested untracked file"
if grep -q '^untracked\.txt$' "$WORK_DIR/ls-files-others-pathspec.out"; then
    fail "git ls-files --others pathspec reported an unrequested untracked path"
fi

cd "$repo" && "${TEST_BIN_DIR}/git" status --short > "$WORK_DIR/status.out"
assert_file_contains "$WORK_DIR/status.out" '^ D deleted.txt$' "git status did not report a deleted tracked file"
assert_file_contains "$WORK_DIR/status.out" '^ M modified.txt$' "git status did not report a modified tracked file"
assert_file_contains "$WORK_DIR/status.out" '^ M script.sh$' "git status did not report an executable-bit change"
assert_file_contains "$WORK_DIR/status.out" '^?? untracked.txt$' "git status did not report an untracked file"
assert_file_contains "$WORK_DIR/status.out" '^?? untracked-dir/nested.txt$' "git status did not report a nested untracked file"
if grep -q '^.. tracked\.txt$' "$WORK_DIR/status.out"; then
    fail "git status reported an unchanged tracked file"
fi
if grep -q 'skip.tmp\|excluded.log\|ignored-dir' "$WORK_DIR/status.out"; then
    fail "git status reported ignored files"
fi

cd "$repo" && "${TEST_BIN_DIR}/git" status --porcelain=v1 -z > "$WORK_DIR/status-porcelain-z.out"
tr '\000' '\n' < "$WORK_DIR/status-porcelain-z.out" > "$WORK_DIR/status-porcelain-z.lines"
assert_file_contains "$WORK_DIR/status-porcelain-z.lines" '^ D deleted.txt$' "git status --porcelain=v1 -z missed a deleted tracked file"
assert_file_contains "$WORK_DIR/status-porcelain-z.lines" '^ M modified.txt$' "git status --porcelain=v1 -z missed a modified tracked file"
assert_file_contains "$WORK_DIR/status-porcelain-z.lines" '^?? untracked.txt$' "git status --porcelain=v1 -z missed an untracked file"

cd "$repo" && "${TEST_BIN_DIR}/git" diff --stat -- deleted.txt modified.txt tracked.txt > "$WORK_DIR/diff-stat.out"
assert_file_contains "$WORK_DIR/diff-stat.out" '^ deleted\.txt  | 1 -$' "git diff --stat did not report a deleted line"
assert_file_contains "$WORK_DIR/diff-stat.out" '^ modified\.txt | 2 +-$' "git diff --stat did not report modified insertions and deletions"
assert_file_contains "$WORK_DIR/diff-stat.out" '^ 2 files changed, 1 insertion(+), 2 deletions(-)$' "git diff --stat did not report totals"
if grep -q 'tracked\.txt' "$WORK_DIR/diff-stat.out"; then
    fail "git diff --stat reported an unchanged path"
fi
cd "$repo" && "${TEST_BIN_DIR}/git" diff --name-status -- deleted.txt modified.txt tracked.txt > "$WORK_DIR/diff-name-status-deleted.out"
assert_file_contains "$WORK_DIR/diff-name-status-deleted.out" '^D[[:space:]]*deleted.txt$' "git diff --name-status did not report a deleted worktree path"
assert_file_contains "$WORK_DIR/diff-name-status-deleted.out" '^M[[:space:]]*modified.txt$' "git diff --name-status did not report a modified worktree path"
if grep -q 'tracked\.txt' "$WORK_DIR/diff-name-status-deleted.out"; then
    fail "git diff --name-status reported an unchanged path"
fi
cd "$repo" && "${TEST_BIN_DIR}/git" diff --name-only -z -- deleted.txt modified.txt tracked.txt > "$WORK_DIR/diff-name-only-z.out"
tr '\000' '\n' < "$WORK_DIR/diff-name-only-z.out" > "$WORK_DIR/diff-name-only-z.lines"
assert_file_contains "$WORK_DIR/diff-name-only-z.lines" '^deleted.txt$' "git diff --name-only -z missed a deleted path"
assert_file_contains "$WORK_DIR/diff-name-only-z.lines" '^modified.txt$' "git diff --name-only -z missed a modified path"
cd "$repo" && "${TEST_BIN_DIR}/git" diff --name-status -z -- deleted.txt modified.txt tracked.txt > "$WORK_DIR/diff-name-status-z.out"
tr '\000' '\n' < "$WORK_DIR/diff-name-status-z.out" > "$WORK_DIR/diff-name-status-z.lines"
assert_file_contains "$WORK_DIR/diff-name-status-z.lines" '^D$' "git diff --name-status -z missed a deleted status field"
assert_file_contains "$WORK_DIR/diff-name-status-z.lines" '^deleted.txt$' "git diff --name-status -z missed a deleted path"
assert_file_contains "$WORK_DIR/diff-name-status-z.lines" '^M$' "git diff --name-status -z missed a modified status field"
assert_file_contains "$WORK_DIR/diff-name-status-z.lines" '^modified.txt$' "git diff --name-status -z missed a modified path"

cd "$repo" && "${TEST_BIN_DIR}/git" --no-pager diff --stat -- modified.txt > "$WORK_DIR/diff-stat-no-pager.out"
assert_file_contains "$WORK_DIR/diff-stat-no-pager.out" '^ modified\.txt | 2 +-$' "git --no-pager diff --stat did not accept the host command shape"
cd "$repo" && "${TEST_BIN_DIR}/git" diff --stat --color=always -- modified.txt > "$WORK_DIR/diff-stat-color.out"
assert_file_contains "$WORK_DIR/diff-stat-color.out" "$(printf '\033\\[32m+\033\\[0m\033\\[31m-\033\\[0m')" "git diff --stat did not color plus and minus bars"
cd "$repo" && "${TEST_BIN_DIR}/git" diff --stat --no-color -- modified.txt > "$WORK_DIR/diff-stat-no-color.out"
if grep -q "$(printf '\033')" "$WORK_DIR/diff-stat-no-color.out"; then
    fail "git diff --stat --no-color emitted ANSI escapes"
fi

cd "$repo" && "${TEST_BIN_DIR}/git" add -N -- untracked.txt untracked-dir
cd "$repo" && "${TEST_BIN_DIR}/git" status --short > "$WORK_DIR/status-intent.out"
assert_file_contains "$WORK_DIR/status-intent.out" '^ A untracked.txt$' "git add -N did not mark an untracked file as intent-to-add"
assert_file_contains "$WORK_DIR/status-intent.out" '^ A untracked-dir/nested.txt$' "git add -N did not mark a nested untracked file as intent-to-add"
if grep -q '^?? untracked' "$WORK_DIR/status-intent.out"; then
    fail "git add -N left intent-to-add paths as untracked"
fi
cd "$repo" && "${TEST_BIN_DIR}/git" ls-files --others --exclude-standard > "$WORK_DIR/ls-files-others-after-add.out"
if grep -q '^untracked' "$WORK_DIR/ls-files-others-after-add.out"; then
    fail "git ls-files --others reported intent-to-add paths"
fi
cd "$repo" && "${TEST_BIN_DIR}/git" diff --stat -- untracked.txt untracked-dir > "$WORK_DIR/diff-stat-intent.out"
assert_file_contains "$WORK_DIR/diff-stat-intent.out" '^ untracked-dir/nested\.txt | 1 +$' "git diff --stat did not report an intent-to-add nested file"
assert_file_contains "$WORK_DIR/diff-stat-intent.out" '^ untracked\.txt            | 1 +$' "git diff --stat did not report an intent-to-add file"
assert_file_contains "$WORK_DIR/diff-stat-intent.out" '^ 2 files changed, 2 insertions(+)$' "git diff --stat did not report intent-to-add totals"

hash_again=$(cd "$repo" && "${TEST_BIN_DIR}/git" hash-object tracked.txt | tr -d '\r\n')
assert_text_equals "$hash_again" "$tracked_oid" "git hash-object was not stable"

stage_repo="$WORK_DIR/stage-repo"
mkdir -p "$stage_repo/.git/refs/heads" "$stage_repo/.git/objects"
stage_info=$(python3 - "$stage_repo" <<'PY'
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

blob = write_object("blob", b"base\n")
tree = write_object("tree", b"100644 tracked.txt\0" + bytes.fromhex(blob))
commit = write_object("commit", ("tree " + tree + "\n\nbase\n").encode())
print(blob + " " + commit)
PY
)
stage_blob=${stage_info%% *}
stage_commit=${stage_info#* }
printf 'ref: refs/heads/main\n' > "$stage_repo/.git/HEAD"
printf '%s\n' "$stage_commit" > "$stage_repo/.git/refs/heads/main"
printf 'base\n' > "$stage_repo/tracked.txt"
{
    printf 'DIRC'
    python3 - <<'PY'
import struct
import sys
sys.stdout.buffer.write(struct.pack('!LL', 2, 1))
PY
    make_index_entry 100644 5 "$stage_blob" tracked.txt
} > "$stage_repo/.git/index"

printf 'worktree\n' > "$stage_repo/tracked.txt"
cd "$stage_repo" && "${TEST_BIN_DIR}/git" status --short > "$WORK_DIR/stage-status-worktree.out"
assert_file_contains "$WORK_DIR/stage-status-worktree.out" '^ M tracked.txt$' "git status did not distinguish an unstaged modification"
cd "$stage_repo" && "${TEST_BIN_DIR}/git" diff -- tracked.txt > "$WORK_DIR/diff-patch-worktree.out"
assert_file_contains "$WORK_DIR/diff-patch-worktree.out" '^diff --git a/tracked.txt b/tracked.txt$' "git diff did not emit a patch header"
assert_file_contains "$WORK_DIR/diff-patch-worktree.out" '^-base$' "git diff patch missed the old line"
assert_file_contains "$WORK_DIR/diff-patch-worktree.out" '^+worktree$' "git diff patch missed the new line"
cd "$stage_repo" && "${TEST_BIN_DIR}/git" diff --name-only -- tracked.txt > "$WORK_DIR/diff-name-only.out"
assert_file_contains "$WORK_DIR/diff-name-only.out" '^tracked.txt$' "git diff --name-only did not report the modified path"
cd "$stage_repo" && "${TEST_BIN_DIR}/git" diff --name-status -- tracked.txt > "$WORK_DIR/diff-name-status.out"
assert_file_contains "$WORK_DIR/diff-name-status.out" '^M[[:space:]]*tracked.txt$' "git diff --name-status did not report an unstaged modification"
if cd "$stage_repo" && "${TEST_BIN_DIR}/git" diff --quiet -- tracked.txt; then
    fail "git diff --quiet returned success for a dirty worktree"
fi
if cd "$stage_repo" && "${TEST_BIN_DIR}/git" diff --exit-code -- tracked.txt > "$WORK_DIR/diff-exit-code.out"; then
    fail "git diff --exit-code returned success for a dirty worktree"
fi

cd "$stage_repo" && "${TEST_BIN_DIR}/git" add tracked.txt
cd "$stage_repo" && "${TEST_BIN_DIR}/git" status --short > "$WORK_DIR/stage-status-staged.out"
assert_file_contains "$WORK_DIR/stage-status-staged.out" '^M  tracked.txt$' "git add did not stage a tracked modification"
cd "$stage_repo" && "${TEST_BIN_DIR}/git" diff -- tracked.txt > "$WORK_DIR/diff-patch-clean-worktree.out"
assert_text_equals "$(cat "$WORK_DIR/diff-patch-clean-worktree.out")" '' "git diff reported a clean worktree after add"
cd "$stage_repo" && "${TEST_BIN_DIR}/git" diff --quiet -- tracked.txt
cd "$stage_repo" && "${TEST_BIN_DIR}/git" diff --exit-code -- tracked.txt > "$WORK_DIR/diff-exit-code-clean.out"
cd "$stage_repo" && "${TEST_BIN_DIR}/git" diff --cached --stat -- tracked.txt > "$WORK_DIR/diff-cached-stat.out"
assert_file_contains "$WORK_DIR/diff-cached-stat.out" '^ tracked.txt | 2 +-$' "git diff --cached --stat did not report a staged modification"
cd "$stage_repo" && "${TEST_BIN_DIR}/git" diff --cached -- tracked.txt > "$WORK_DIR/diff-cached-patch.out"
assert_file_contains "$WORK_DIR/diff-cached-patch.out" '^+worktree$' "git diff --cached patch missed staged content"
cd "$stage_repo" && "${TEST_BIN_DIR}/git" diff --cached --name-status -- tracked.txt > "$WORK_DIR/diff-cached-name-status.out"
assert_file_contains "$WORK_DIR/diff-cached-name-status.out" '^M[[:space:]]*tracked.txt$' "git diff --cached --name-status did not report a staged modification"
if cd "$stage_repo" && "${TEST_BIN_DIR}/git" diff --cached --quiet -- tracked.txt; then
    fail "git diff --cached --quiet returned success for a staged modification"
fi

printf 'again\n' > "$stage_repo/tracked.txt"
cd "$stage_repo" && "${TEST_BIN_DIR}/git" status --short > "$WORK_DIR/stage-status-mm.out"
assert_file_contains "$WORK_DIR/stage-status-mm.out" '^MM tracked.txt$' "git status did not report a staged and unstaged modification"
printf 'fresh\n' > "$stage_repo/new.txt"
cd "$stage_repo" && "${TEST_BIN_DIR}/git" add new.txt
cd "$stage_repo" && "${TEST_BIN_DIR}/git" status --short > "$WORK_DIR/stage-status-added.out"
assert_file_contains "$WORK_DIR/stage-status-added.out" '^A  new.txt$' "git add did not stage a new file"

rm -f "$stage_repo/tracked.txt"
cd "$stage_repo" && "${TEST_BIN_DIR}/git" add tracked.txt
cd "$stage_repo" && "${TEST_BIN_DIR}/git" status --short > "$WORK_DIR/stage-status-deleted.out"
assert_file_contains "$WORK_DIR/stage-status-deleted.out" '^D  tracked.txt$' "git add did not stage a tracked deletion"

commit_repo="$WORK_DIR/commit-repo"
mkdir -p "$commit_repo/.git/refs/heads" "$commit_repo/.git/objects"
printf 'ref: refs/heads/main\n' > "$commit_repo/.git/HEAD"
printf 'one\n' > "$commit_repo/a.txt"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" add a.txt
cd "$commit_repo" && GIT_AUTHOR_NAME=A GIT_AUTHOR_EMAIL=a@example.invalid GIT_COMMITTER_NAME=A GIT_COMMITTER_EMAIL=a@example.invalid "${TEST_BIN_DIR}/git" commit -m first > "$WORK_DIR/commit-first.out"
assert_file_contains "$WORK_DIR/commit-first.out" '^Committed [0-9a-f][0-9a-f]' "git commit did not report a written commit"
first_commit=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" rev-parse HEAD | tr -d '\r\n')
if [ ${#first_commit} -ne 40 ]; then
    fail "git commit did not update HEAD to a full object id"
fi
cd "$commit_repo" && "${TEST_BIN_DIR}/git" status --short > "$WORK_DIR/commit-status-clean.out"
assert_text_equals "$(cat "$WORK_DIR/commit-status-clean.out")" '' "git commit destination should be clean"

printf 'two\n' > "$commit_repo/a.txt"
printf 'new\n' > "$commit_repo/b.txt"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" add a.txt b.txt
cd "$commit_repo" && GIT_AUTHOR_NAME=A GIT_AUTHOR_EMAIL=a@example.invalid GIT_COMMITTER_NAME=A GIT_COMMITTER_EMAIL=a@example.invalid "${TEST_BIN_DIR}/git" commit -m second > "$WORK_DIR/commit-second.out"
second_commit=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" rev-parse HEAD | tr -d '\r\n')
if [ "$first_commit" = "$second_commit" ]; then
    fail "git commit did not advance HEAD for a second commit"
fi
cd "$commit_repo" && "${TEST_BIN_DIR}/git" diff --stat "$first_commit" "$second_commit" > "$WORK_DIR/diff-commits-stat.out"
assert_file_contains "$WORK_DIR/diff-commits-stat.out" '^ a\.txt | 2 +-$' "git diff <commit> <commit> did not report a modified file"
assert_file_contains "$WORK_DIR/diff-commits-stat.out" '^ b\.txt | 1 +$' "git diff <commit> <commit> did not report an added file"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" diff "$first_commit..$second_commit" -- b.txt > "$WORK_DIR/diff-commits-range.out"
assert_file_contains "$WORK_DIR/diff-commits-range.out" '^+new$' "git diff commit range did not render the added file patch"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" diff --name-only "$first_commit" "$second_commit" > "$WORK_DIR/diff-commits-name-only.out"
assert_file_contains "$WORK_DIR/diff-commits-name-only.out" '^a.txt$' "git diff --name-only missed a modified commit path"
assert_file_contains "$WORK_DIR/diff-commits-name-only.out" '^b.txt$' "git diff --name-only missed an added commit path"

merge_base=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" merge-base "$first_commit" "$second_commit" | tr -d '\r\n')
assert_text_equals "$merge_base" "$first_commit" "git merge-base did not find the first-parent common ancestor"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" merge-base --is-ancestor "$first_commit" "$second_commit"
if cd "$commit_repo" && "${TEST_BIN_DIR}/git" merge-base --is-ancestor "$second_commit" "$first_commit"; then
    fail "git merge-base --is-ancestor accepted a descendant as an ancestor"
fi
rev_count=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" rev-list --count "$first_commit..$second_commit" | tr -d '\r\n')
assert_text_equals "$rev_count" '1' "git rev-list --count A..B did not count first-parent commits"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" log --format='%H %s %an %ae' -n 1 > "$WORK_DIR/log-format.out"
assert_file_contains "$WORK_DIR/log-format.out" "^$second_commit second A a@example\.invalid$" "git log --format did not expand the supported placeholders"
second_tree=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" cat-file -p "$second_commit" | sed -n 's/^tree //p' | head -n 1)
cd "$commit_repo" && "${TEST_BIN_DIR}/git" branch side "$first_commit"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" switch side > "$WORK_DIR/switch-side.out"
printf 'side\n' > "$commit_repo/c.txt"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" add c.txt
cd "$commit_repo" && GIT_AUTHOR_NAME=S GIT_AUTHOR_EMAIL=s@example.invalid GIT_COMMITTER_NAME=S GIT_COMMITTER_EMAIL=s@example.invalid "${TEST_BIN_DIR}/git" commit -m side > "$WORK_DIR/commit-side.out"
side_commit=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" rev-parse HEAD | tr -d '\r\n')
merge_commit=$(python3 - "$commit_repo" "$second_tree" "$side_commit" "$second_commit" <<'PY'
import hashlib
import os
import sys
import zlib

repo, tree, first_parent, second_parent = sys.argv[1:]
payload = (f"tree {tree}\nparent {first_parent}\nparent {second_parent}\n"
           "author M <m@example.invalid> 0 +0000\n"
           "committer M <m@example.invalid> 0 +0000\n\nmerge\n").encode()
full = b"commit " + str(len(payload)).encode() + b"\0" + payload
oid = hashlib.sha1(full).hexdigest()
directory = os.path.join(repo, ".git", "objects", oid[:2])
os.makedirs(directory, exist_ok=True)
with open(os.path.join(directory, oid[2:]), "wb") as handle:
    handle.write(zlib.compress(full))
print(oid)
PY
)
printf '%s\n' "$merge_commit" > "$commit_repo/.git/refs/heads/synthetic-merge"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" switch main > "$WORK_DIR/switch-main-after-dag.out"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" merge-base --is-ancestor "$second_commit" "$merge_commit"
dag_base=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" merge-base "$second_commit" "$merge_commit" | tr -d '\r\n')
assert_text_equals "$dag_base" "$second_commit" "git merge-base did not traverse a merge commit second parent"
dag_count=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" rev-list --count "$side_commit..$merge_commit" | tr -d '\r\n')
assert_text_equals "$dag_count" '2' "git rev-list --count A..B did not count non-first-parent reachable commits"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" log --format='%P' -n 1 "$merge_commit" > "$WORK_DIR/log-parents.out"
assert_file_contains "$WORK_DIR/log-parents.out" "^$side_commit $second_commit$" "git log --format=%P did not print all commit parents"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" reflog > "$WORK_DIR/reflog.out"
assert_file_contains "$WORK_DIR/reflog.out" 'HEAD@.0.: checkout' "git reflog did not show the latest HEAD checkout"
assert_file_contains "$WORK_DIR/reflog.out" 'commit' "git reflog did not record commits"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" blame -- a.txt > "$WORK_DIR/blame.out"
assert_file_contains "$WORK_DIR/blame.out" 'A <a@example.invalid>' "git blame did not print commit author information"
symbolic_head=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" symbolic-ref HEAD | tr -d '\r\n')
assert_text_equals "$symbolic_head" 'refs/heads/main' "git symbolic-ref HEAD did not print the full HEAD ref"
symbolic_short=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" symbolic-ref --short HEAD | tr -d '\r\n')
assert_text_equals "$symbolic_short" 'main' "git symbolic-ref --short HEAD did not abbreviate refs/heads"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" update-ref refs/heads/alias "$first_commit"
alias_ref=$(tr -d '\r\n' < "$commit_repo/.git/refs/heads/alias")
assert_text_equals "$alias_ref" "$first_commit" "git update-ref did not write the requested ref"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" for-each-ref --format='%(refname:short) %(objectname:short)' refs/heads > "$WORK_DIR/for-each-ref.out"
assert_file_contains "$WORK_DIR/for-each-ref.out" "^alias $(printf '%s' "$first_commit" | cut -c1-7)$" "git for-each-ref did not format a loose head ref"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" update-ref -d refs/heads/alias
if [ -e "$commit_repo/.git/refs/heads/alias" ]; then
    fail "git update-ref -d did not delete a loose ref"
fi

cd "$commit_repo" && "${TEST_BIN_DIR}/git" branch > "$WORK_DIR/branch-list.out"
assert_file_contains "$WORK_DIR/branch-list.out" '^\* main$' "git branch did not mark the current branch"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" branch topic "$first_commit"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" branch > "$WORK_DIR/branch-created.out"
assert_file_contains "$WORK_DIR/branch-created.out" '^  topic$' "git branch did not list a created branch"
topic_commit=$(tr -d '\r\n' < "$commit_repo/.git/refs/heads/topic")
assert_text_equals "$topic_commit" "$first_commit" "git branch did not create the branch at the requested start point"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" branch -d topic > "$WORK_DIR/branch-delete.out"
assert_file_contains "$WORK_DIR/branch-delete.out" '^Deleted branch topic$' "git branch -d did not report deletion"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" checkout -b topic2 "$first_commit" > "$WORK_DIR/checkout-b.out"
topic2_branch=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" branch --show-current | tr -d '\r\n')
assert_text_equals "$topic2_branch" 'topic2' "git checkout -b did not switch to the created branch"
topic2_head=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" rev-parse --verify --short=12 HEAD | tr -d '\r\n')
assert_text_equals "$topic2_head" "$(printf '%s' "$first_commit" | cut -c1-12)" "git rev-parse --verify --short did not abbreviate HEAD"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" switch main > "$WORK_DIR/switch-main.out"
main_branch=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" branch --show-current | tr -d '\r\n')
assert_text_equals "$main_branch" 'main' "git switch did not switch back to main"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" switch -c topic3 "$first_commit" > "$WORK_DIR/switch-create.out"
topic3_branch=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" branch --show-current | tr -d '\r\n')
assert_text_equals "$topic3_branch" 'topic3' "git switch -c did not switch to the created branch"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" switch main > "$WORK_DIR/switch-main-again.out"

cd "$commit_repo" && "${TEST_BIN_DIR}/git" show-ref --verify refs/heads/main > "$WORK_DIR/show-ref-main.out"
assert_file_contains "$WORK_DIR/show-ref-main.out" "^$second_commit refs/heads/main$" "git show-ref --verify did not print refs/heads/main"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" show-ref --heads > "$WORK_DIR/show-ref-heads.out"
assert_file_contains "$WORK_DIR/show-ref-heads.out" "^$second_commit refs/heads/main$" "git show-ref --heads did not include refs/heads/main"
if cd "$commit_repo" && "${TEST_BIN_DIR}/git" show-ref --verify refs/heads/missing > "$WORK_DIR/show-ref-missing.out"; then
    fail "git show-ref --verify returned success for a missing ref"
fi
cd "$commit_repo" && "${TEST_BIN_DIR}/git" tag v-first "$first_commit"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" tag > "$WORK_DIR/tag-list.out"
assert_file_contains "$WORK_DIR/tag-list.out" '^v-first$' "git tag did not list a lightweight tag"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" show-ref --tags > "$WORK_DIR/show-ref-tags.out"
assert_file_contains "$WORK_DIR/show-ref-tags.out" "^$first_commit refs/tags/v-first$" "git show-ref --tags did not include the lightweight tag"
rm -f "$commit_repo/.git/refs/tags/v-first"
printf '%s refs/tags/v-packed\n' "$first_commit" > "$commit_repo/.git/packed-refs"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" tag > "$WORK_DIR/tag-packed-list.out"
assert_file_contains "$WORK_DIR/tag-packed-list.out" '^v-packed$' "git tag did not list a packed lightweight tag"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" show-ref --tags > "$WORK_DIR/show-ref-packed-tags.out"
assert_file_contains "$WORK_DIR/show-ref-packed-tags.out" "^$first_commit refs/tags/v-packed$" "git show-ref --tags did not include a packed tag"
cat_type=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" cat-file -t HEAD | tr -d '\r\n')
assert_text_equals "$cat_type" 'commit' "git cat-file -t HEAD did not report commit"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" cat-file -p HEAD > "$WORK_DIR/cat-file-head.out"
assert_file_contains "$WORK_DIR/cat-file-head.out" '^tree [0-9a-f][0-9a-f]' "git cat-file -p HEAD did not print commit content"
blob_oid=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" hash-object b.txt | tr -d '\r\n')
blob_type=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" cat-file -t "$blob_oid" | tr -d '\r\n')
assert_text_equals "$blob_type" 'blob' "git cat-file -t BLOB did not report blob"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" cat-file -p "$blob_oid" > "$WORK_DIR/cat-file-blob.out"
assert_file_contains "$WORK_DIR/cat-file-blob.out" '^new$' "git cat-file -p BLOB did not print blob data"
tree_oid=$(sed -n 's/^tree //p' "$WORK_DIR/cat-file-head.out" | head -n 1)
tree_type=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" cat-file -t "$tree_oid" | tr -d '\r\n')
assert_text_equals "$tree_type" 'tree' "git cat-file -t TREE did not report tree"
tree_size=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" cat-file -s "$tree_oid" | tr -d '\r\n')
if [ -z "$tree_size" ] || [ "$tree_size" = 0 ]; then
    fail "git cat-file -s TREE did not print a positive size"
fi
cd "$commit_repo" && "${TEST_BIN_DIR}/git" cat-file -p "$tree_oid" > "$WORK_DIR/cat-file-tree.out"
assert_file_contains "$WORK_DIR/cat-file-tree.out" '100644 blob [0-9a-f][0-9a-f].*[[:space:]]a.txt$' "git cat-file -p TREE did not list tree entries"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" ls-tree HEAD > "$WORK_DIR/ls-tree.out"
assert_file_contains "$WORK_DIR/ls-tree.out" '100644 blob [0-9a-f][0-9a-f].*[[:space:]]a.txt$' "git ls-tree did not list a.txt"
assert_file_contains "$WORK_DIR/ls-tree.out" '100644 blob [0-9a-f][0-9a-f].*[[:space:]]b.txt$' "git ls-tree did not list b.txt"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" ls-tree -r --name-only HEAD > "$WORK_DIR/ls-tree-recursive-name-only.out"
assert_file_contains "$WORK_DIR/ls-tree-recursive-name-only.out" '^a.txt$' "git ls-tree -r --name-only did not list a.txt"
assert_file_contains "$WORK_DIR/ls-tree-recursive-name-only.out" '^b.txt$' "git ls-tree -r --name-only did not list b.txt"

cd "$commit_repo" && "${TEST_BIN_DIR}/git" log --oneline -n 1 > "$WORK_DIR/log-one.out"
assert_file_contains "$WORK_DIR/log-one.out" "^$(printf '%s' "$second_commit" | cut -c1-7) second$" "git log --oneline -n 1 did not show the latest commit"
if grep -q 'first' "$WORK_DIR/log-one.out"; then
    fail "git log --oneline -n 1 reported too many commits"
fi
cd "$commit_repo" && "${TEST_BIN_DIR}/git" log --oneline --max-count=2 > "$WORK_DIR/log-two.out"
assert_file_contains "$WORK_DIR/log-two.out" ' second$' "git log --max-count=2 missed the second commit"
assert_file_contains "$WORK_DIR/log-two.out" ' first$' "git log --max-count=2 missed the first commit"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" show --stat HEAD > "$WORK_DIR/show-stat.out"
assert_file_contains "$WORK_DIR/show-stat.out" '^commit ' "git show --stat did not print a commit header"
assert_file_contains "$WORK_DIR/show-stat.out" '^ a\.txt | 2 +-$' "git show --stat did not report a modified file"
assert_file_contains "$WORK_DIR/show-stat.out" '^ b\.txt | 1 +$' "git show --stat did not report an added file"

printf 'dirty\n' > "$commit_repo/a.txt"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" restore --worktree a.txt
assert_file_contains "$commit_repo/a.txt" '^two$' "git restore --worktree did not restore a file from the index"
printf 'staged\n' > "$commit_repo/a.txt"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" add a.txt
cd "$commit_repo" && "${TEST_BIN_DIR}/git" restore --staged a.txt
cd "$commit_repo" && "${TEST_BIN_DIR}/git" status --short > "$WORK_DIR/restore-staged-status.out"
assert_file_contains "$WORK_DIR/restore-staged-status.out" '^ M a.txt$' "git restore --staged did not unstage a file while keeping the worktree change"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" restore --worktree a.txt
assert_file_contains "$commit_repo/a.txt" '^two$' "git restore --worktree did not restore a file after unstaging"

cd "$commit_repo" && "${TEST_BIN_DIR}/git" rm --cached b.txt
cd "$commit_repo" && "${TEST_BIN_DIR}/git" status --short > "$WORK_DIR/rm-cached-status.out"
assert_file_contains "$WORK_DIR/rm-cached-status.out" '^D  b.txt$' "git rm --cached did not stage a deletion"
assert_file_contains "$WORK_DIR/rm-cached-status.out" '^?? b.txt$' "git rm --cached did not leave the worktree file untracked"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" restore --staged b.txt
cd "$commit_repo" && "${TEST_BIN_DIR}/git" rm b.txt
if [ -e "$commit_repo/b.txt" ]; then
    fail "git rm did not remove the worktree file"
fi
cd "$commit_repo" && "${TEST_BIN_DIR}/git" status --short > "$WORK_DIR/rm-status.out"
assert_file_contains "$WORK_DIR/rm-status.out" '^D  b.txt$' "git rm did not stage a deletion"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" restore --staged --worktree b.txt
assert_file_contains "$commit_repo/b.txt" '^new$' "git restore --staged --worktree did not restore a removed file"

printf 'three\n' > "$commit_repo/a.txt"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" add a.txt
cd "$commit_repo" && GIT_AUTHOR_NAME=A GIT_AUTHOR_EMAIL=a@example.invalid GIT_COMMITTER_NAME=A GIT_COMMITTER_EMAIL=a@example.invalid "${TEST_BIN_DIR}/git" commit -m third > "$WORK_DIR/commit-third.out"
third_commit=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" rev-parse HEAD | tr -d '\r\n')
if [ "$third_commit" = "$second_commit" ]; then
    fail "git commit did not create a third commit"
fi
cd "$commit_repo" && "${TEST_BIN_DIR}/git" reset --mixed "$second_commit" > "$WORK_DIR/reset-mixed.out"
reset_head=$(cd "$commit_repo" && "${TEST_BIN_DIR}/git" rev-parse HEAD | tr -d '\r\n')
assert_text_equals "$reset_head" "$second_commit" "git reset --mixed did not move HEAD"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" status --short > "$WORK_DIR/reset-mixed-status.out"
assert_file_contains "$WORK_DIR/reset-mixed-status.out" '^ M a.txt$' "git reset --mixed did not leave the worktree change unstaged"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" reset --hard "$first_commit" > "$WORK_DIR/reset-hard-first.out"
if [ -e "$commit_repo/b.txt" ]; then
    fail "git reset --hard did not remove a file absent from the target commit"
fi
assert_file_contains "$commit_repo/a.txt" '^one$' "git reset --hard did not restore target commit content"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" reset --hard "$second_commit" > "$WORK_DIR/reset-hard-second.out"
assert_file_contains "$commit_repo/a.txt" '^two$' "git reset --hard did not restore the later commit content"
assert_file_contains "$commit_repo/b.txt" '^new$' "git reset --hard did not restore a file from the later commit"

mkdir -p "$commit_repo/.git/info"
printf '*.tmp\n' > "$commit_repo/.git/info/exclude"
printf 'loose\n' > "$commit_repo/loose.txt"
printf 'ignored\n' > "$commit_repo/skip.tmp"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" clean -n > "$WORK_DIR/clean-dry-run.out"
assert_file_contains "$WORK_DIR/clean-dry-run.out" '^Would remove loose.txt$' "git clean -n did not report an untracked file"
if grep -q 'skip.tmp' "$WORK_DIR/clean-dry-run.out"; then
    fail "git clean -n reported an ignored file by default"
fi
cd "$commit_repo" && "${TEST_BIN_DIR}/git" clean -f -- loose.txt > "$WORK_DIR/clean-force.out"
if [ -e "$commit_repo/loose.txt" ]; then
    fail "git clean -f did not remove an untracked file"
fi
if [ ! -e "$commit_repo/skip.tmp" ]; then
    fail "git clean -f removed an ignored file without -x"
fi
rm -f "$commit_repo/skip.tmp"

cd "$commit_repo" && "${TEST_BIN_DIR}/git" checkout "$first_commit" > "$WORK_DIR/checkout-cleanup.out"
if [ -e "$commit_repo/b.txt" ]; then
    fail "git checkout did not remove a path absent from the target tree"
fi
assert_file_contains "$commit_repo/a.txt" '^one$' "git checkout did not restore the target tree content"

mkdir -p "$commit_repo/sub" "$commit_repo/other"
printf '*.log\n!keep.log\n' > "$commit_repo/sub/.gitignore"
printf 'ignored\n' > "$commit_repo/sub/drop.log"
printf 'kept\n' > "$commit_repo/sub/keep.log"
printf 'visible\n' > "$commit_repo/other/drop.log"
cd "$commit_repo" && "${TEST_BIN_DIR}/git" ls-files --others --exclude-standard > "$WORK_DIR/nested-ignore.out"
assert_file_contains "$WORK_DIR/nested-ignore.out" '^other/drop\.log$' "git ls-files missed an untracked file outside a nested ignore scope"
assert_file_contains "$WORK_DIR/nested-ignore.out" '^sub/keep\.log$' "git ls-files missed a nested ignore negation"
if grep -q '^sub/drop\.log$' "$WORK_DIR/nested-ignore.out"; then
    fail "git ls-files reported a file ignored by nested .gitignore"
fi

hook_repo="$WORK_DIR/hook-repo"
"${TEST_BIN_DIR}/git" init "$hook_repo" > "$WORK_DIR/hook-init.out"
printf 'hooked\n' > "$hook_repo/h.txt"
cd "$hook_repo" && "${TEST_BIN_DIR}/git" add h.txt
mkdir -p "$hook_repo/.git/hooks"
cat > "$hook_repo/.git/hooks/pre-commit" <<'SH'
#!/bin/sh
printf pre > .git/pre-commit-ran
SH
chmod 755 "$hook_repo/.git/hooks/pre-commit"
cat > "$hook_repo/.git/hooks/commit-msg" <<'SH'
#!/bin/sh
grep -q editor-message "$1"
SH
chmod 755 "$hook_repo/.git/hooks/commit-msg"
cat > "$WORK_DIR/editor.sh" <<'SH'
#!/bin/sh
printf 'editor-message\n' > "$1"
SH
chmod 755 "$WORK_DIR/editor.sh"
cd "$hook_repo" && GIT_EDITOR="$WORK_DIR/editor.sh" GIT_AUTHOR_NAME=H GIT_AUTHOR_EMAIL=h@example.invalid GIT_COMMITTER_NAME=H GIT_COMMITTER_EMAIL=h@example.invalid "${TEST_BIN_DIR}/git" commit > "$WORK_DIR/hook-commit.out"
assert_file_contains "$hook_repo/.git/pre-commit-ran" '^pre$' "git commit did not run pre-commit hook"
cd "$hook_repo" && "${TEST_BIN_DIR}/git" log --format='%s' -n 1 > "$WORK_DIR/hook-log.out"
assert_file_contains "$WORK_DIR/hook-log.out" '^editor-message$' "git commit did not read the editor-written message"

ff_repo="$WORK_DIR/ff-repo"
"${TEST_BIN_DIR}/git" init "$ff_repo" > "$WORK_DIR/ff-init.out"
printf 'base\n' > "$ff_repo/f.txt"
cd "$ff_repo" && "${TEST_BIN_DIR}/git" add f.txt
cd "$ff_repo" && GIT_AUTHOR_NAME=F GIT_AUTHOR_EMAIL=f@example.invalid GIT_COMMITTER_NAME=F GIT_COMMITTER_EMAIL=f@example.invalid "${TEST_BIN_DIR}/git" commit -m base > "$WORK_DIR/ff-base.out"
base_ff=$(cd "$ff_repo" && "${TEST_BIN_DIR}/git" rev-parse HEAD | tr -d '\r\n')
cd "$ff_repo" && "${TEST_BIN_DIR}/git" branch next "$base_ff"
cd "$ff_repo" && "${TEST_BIN_DIR}/git" switch next > "$WORK_DIR/ff-switch-next.out"
printf 'next\n' > "$ff_repo/f.txt"
cd "$ff_repo" && "${TEST_BIN_DIR}/git" add f.txt
cd "$ff_repo" && GIT_AUTHOR_NAME=F GIT_AUTHOR_EMAIL=f@example.invalid GIT_COMMITTER_NAME=F GIT_COMMITTER_EMAIL=f@example.invalid "${TEST_BIN_DIR}/git" commit -m next > "$WORK_DIR/ff-next.out"
next_ff=$(cd "$ff_repo" && "${TEST_BIN_DIR}/git" rev-parse HEAD | tr -d '\r\n')
cd "$ff_repo" && "${TEST_BIN_DIR}/git" switch main > "$WORK_DIR/ff-switch-main.out"
cd "$ff_repo" && "${TEST_BIN_DIR}/git" merge next > "$WORK_DIR/ff-merge.out"
merged_ff=$(cd "$ff_repo" && "${TEST_BIN_DIR}/git" rev-parse HEAD | tr -d '\r\n')
assert_text_equals "$merged_ff" "$next_ff" "git merge did not fast-forward to the target commit"
assert_file_contains "$ff_repo/f.txt" '^next$' "git merge did not update the worktree during fast-forward"

push_remote="$WORK_DIR/push-remote"
"${TEST_BIN_DIR}/git" init "$push_remote" > "$WORK_DIR/push-remote-init.out"
cd "$ff_repo" && "${TEST_BIN_DIR}/git" push "$push_remote/.git" main > "$WORK_DIR/push-local.out"
push_ref=$(tr -d '\r\n' < "$push_remote/.git/refs/heads/main")
assert_text_equals "$push_ref" "$next_ff" "git push did not update a local remote ref"

cp_repo="$WORK_DIR/cp-repo"
"${TEST_BIN_DIR}/git" init "$cp_repo" > "$WORK_DIR/cp-init.out"
printf 'one\n' > "$cp_repo/x.txt"
cd "$cp_repo" && "${TEST_BIN_DIR}/git" add x.txt
cd "$cp_repo" && GIT_AUTHOR_NAME=C GIT_AUTHOR_EMAIL=c@example.invalid GIT_COMMITTER_NAME=C GIT_COMMITTER_EMAIL=c@example.invalid "${TEST_BIN_DIR}/git" commit -m one > "$WORK_DIR/cp-one.out"
cp_first=$(cd "$cp_repo" && "${TEST_BIN_DIR}/git" rev-parse HEAD | tr -d '\r\n')
printf 'two\n' > "$cp_repo/x.txt"
cd "$cp_repo" && "${TEST_BIN_DIR}/git" add x.txt
cd "$cp_repo" && GIT_AUTHOR_NAME=C GIT_AUTHOR_EMAIL=c@example.invalid GIT_COMMITTER_NAME=C GIT_COMMITTER_EMAIL=c@example.invalid "${TEST_BIN_DIR}/git" commit -m two > "$WORK_DIR/cp-two.out"
cp_second=$(cd "$cp_repo" && "${TEST_BIN_DIR}/git" rev-parse HEAD | tr -d '\r\n')
cd "$cp_repo" && "${TEST_BIN_DIR}/git" reset --hard "$cp_first" > "$WORK_DIR/cp-reset.out"
cd "$cp_repo" && "${TEST_BIN_DIR}/git" cherry-pick "$cp_second" > "$WORK_DIR/cp-cherry.out"
cp_pick=$(cd "$cp_repo" && "${TEST_BIN_DIR}/git" rev-parse HEAD | tr -d '\r\n')
assert_text_equals "$cp_pick" "$cp_second" "git cherry-pick did not fast-forward a direct child commit"
cd "$cp_repo" && "${TEST_BIN_DIR}/git" revert HEAD > "$WORK_DIR/cp-revert.out"
assert_file_contains "$cp_repo/x.txt" '^one$' "git revert HEAD did not restore the parent tree"
revert_subject=$(cd "$cp_repo" && "${TEST_BIN_DIR}/git" log --format='%s' -n 1 | tr -d '\r\n')
assert_text_equals "$revert_subject" 'Revert HEAD' "git revert HEAD did not create a revert commit"

diff_quality_repo="$WORK_DIR/diff-quality-repo"
"${TEST_BIN_DIR}/git" init "$diff_quality_repo" > "$WORK_DIR/diff-quality-init.out"
for n in 1 2 3 4 5 6 7 8 9 10; do printf 'line%s\n' "$n"; done > "$diff_quality_repo/lines.txt"
printf 'same\n' > "$diff_quality_repo/old-name.txt"
cd "$diff_quality_repo" && "${TEST_BIN_DIR}/git" add lines.txt old-name.txt
cd "$diff_quality_repo" && GIT_AUTHOR_NAME=D GIT_AUTHOR_EMAIL=d@example.invalid GIT_COMMITTER_NAME=D GIT_COMMITTER_EMAIL=d@example.invalid "${TEST_BIN_DIR}/git" commit -m base > "$WORK_DIR/diff-quality-base.out"
python3 - "$diff_quality_repo/lines.txt" <<'PY'
import sys
path = sys.argv[1]
lines = [f"line{i}\n" for i in range(1, 11)]
lines[5] = "changed6\n"
with open(path, "w", encoding="utf-8") as handle:
    handle.writelines(lines)
PY
cd "$diff_quality_repo" && "${TEST_BIN_DIR}/git" diff -- lines.txt > "$WORK_DIR/diff-compact.out"
assert_file_contains "$WORK_DIR/diff-compact.out" '^ line3$' "git diff compact hunk did not include leading context"
assert_file_contains "$WORK_DIR/diff-compact.out" '^-line6$' "git diff compact hunk did not include removed line"
assert_file_contains "$WORK_DIR/diff-compact.out" '^+changed6$' "git diff compact hunk did not include inserted line"
if grep -q '^ line1$' "$WORK_DIR/diff-compact.out"; then
    fail "git diff compact hunk included distant unchanged prefix lines"
fi
mv "$diff_quality_repo/old-name.txt" "$diff_quality_repo/new-name.txt"
cd "$diff_quality_repo" && "${TEST_BIN_DIR}/git" add old-name.txt new-name.txt
cd "$diff_quality_repo" && "${TEST_BIN_DIR}/git" diff --cached --name-status -z > "$WORK_DIR/diff-rename-z.out"
tr '\000' '\n' < "$WORK_DIR/diff-rename-z.out" > "$WORK_DIR/diff-rename-z.lines"
assert_file_contains "$WORK_DIR/diff-rename-z.lines" '^R100$' "git diff --name-status -z did not report an exact rename status"
assert_file_contains "$WORK_DIR/diff-rename-z.lines" '^old-name.txt$' "git diff --name-status -z did not include the old rename path"
assert_file_contains "$WORK_DIR/diff-rename-z.lines" '^new-name.txt$' "git diff --name-status -z did not include the new rename path"
cd "$diff_quality_repo" && "${TEST_BIN_DIR}/git" diff --cached > "$WORK_DIR/diff-rename-patch.out"
assert_file_contains "$WORK_DIR/diff-rename-patch.out" '^similarity index 100%$' "git diff patch did not include exact rename metadata"
assert_file_contains "$WORK_DIR/diff-rename-patch.out" '^rename from old-name.txt$' "git diff patch did not include rename-from metadata"
assert_file_contains "$WORK_DIR/diff-rename-patch.out" '^rename to new-name.txt$' "git diff patch did not include rename-to metadata"

blame_history_repo="$WORK_DIR/blame-history-repo"
"${TEST_BIN_DIR}/git" init "$blame_history_repo" > "$WORK_DIR/blame-history-init.out"
printf 'keep\nold\n' > "$blame_history_repo/file.txt"
cd "$blame_history_repo" && "${TEST_BIN_DIR}/git" add file.txt
cd "$blame_history_repo" && GIT_AUTHOR_NAME=Old GIT_AUTHOR_EMAIL=old@example.invalid GIT_COMMITTER_NAME=Old GIT_COMMITTER_EMAIL=old@example.invalid "${TEST_BIN_DIR}/git" commit -m old > "$WORK_DIR/blame-history-old.out"
printf 'keep\nnew\n' > "$blame_history_repo/file.txt"
cd "$blame_history_repo" && "${TEST_BIN_DIR}/git" add file.txt
cd "$blame_history_repo" && GIT_AUTHOR_NAME=New GIT_AUTHOR_EMAIL=new@example.invalid GIT_COMMITTER_NAME=New GIT_COMMITTER_EMAIL=new@example.invalid "${TEST_BIN_DIR}/git" commit -m new > "$WORK_DIR/blame-history-new.out"
cd "$blame_history_repo" && "${TEST_BIN_DIR}/git" blame -- file.txt > "$WORK_DIR/blame-history.out"
assert_file_contains "$WORK_DIR/blame-history.out" 'Old <old@example.invalid> .* 1).*keep$' "git blame did not attribute an unchanged line to the older commit"
assert_file_contains "$WORK_DIR/blame-history.out" 'New <new@example.invalid> .* 2).*new$' "git blame did not attribute a changed line to the newer commit"

apply_repo="$WORK_DIR/apply-repo"
"${TEST_BIN_DIR}/git" init "$apply_repo" > "$WORK_DIR/apply-init.out"
printf 'old\nkeep\n' > "$apply_repo/a.txt"
cat > "$WORK_DIR/change.patch" <<'PATCH'
diff --git a/a.txt b/a.txt
--- a/a.txt
+++ b/a.txt
@@ -1,2 +1,2 @@
-old
+new
 keep
diff --git a/new.txt b/new.txt
--- /dev/null
+++ b/new.txt
@@ -0,0 +1,1 @@
+created
PATCH
"${TEST_BIN_DIR}/git" -C "$apply_repo" apply --check "$WORK_DIR/change.patch"
assert_file_contains "$apply_repo/a.txt" '^old$' "git apply --check mutated an existing file"
if [ -e "$apply_repo/new.txt" ]; then
    fail "git apply --check created a new file"
fi
"${TEST_BIN_DIR}/git" -C "$apply_repo" apply "$WORK_DIR/change.patch"
assert_file_contains "$apply_repo/a.txt" '^new$' "git apply did not update an existing file"
assert_file_contains "$apply_repo/a.txt" '^keep$' "git apply did not preserve context lines"
assert_file_contains "$apply_repo/new.txt" '^created$' "git apply did not create a new file"
cat > "$WORK_DIR/delete.patch" <<'PATCH'
diff --git a/new.txt b/new.txt
--- a/new.txt
+++ /dev/null
@@ -1,1 +0,0 @@
-created
PATCH
"${TEST_BIN_DIR}/git" -C "$apply_repo" apply "$WORK_DIR/delete.patch"
if [ -e "$apply_repo/new.txt" ]; then
    fail "git apply did not delete a removed file"
fi
cat > "$WORK_DIR/bad-context.patch" <<'PATCH'
diff --git a/a.txt b/a.txt
--- a/a.txt
+++ b/a.txt
@@ -1,2 +1,2 @@
-missing
+wrong
 keep
PATCH
if "${TEST_BIN_DIR}/git" -C "$apply_repo" apply "$WORK_DIR/bad-context.patch" > "$WORK_DIR/apply-bad.out" 2>/dev/null; then
    fail "git apply accepted a patch whose context did not match"
fi
if "${TEST_BIN_DIR}/git" -C "$apply_repo" apply --check "$WORK_DIR/bad-context.patch" > "$WORK_DIR/apply-check-bad.out" 2>/dev/null; then
    fail "git apply --check accepted a patch whose context did not match"
fi
assert_file_contains "$apply_repo/a.txt" '^new$' "git apply changed a file after a failed context match"

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