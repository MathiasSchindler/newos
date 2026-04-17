#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"

WORK_DIR="$ROOT_DIR/tests/tmp/extended_tools"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/sub"

note "extended tools"

"$ROOT_DIR/build/date" > "$WORK_DIR/date.out"
assert_file_contains "$WORK_DIR/date.out" 'UTC$' "date output missing UTC suffix"
date_fmt_out=$("$ROOT_DIR/build/date" +%Y-%m-%d | tr -d '\r\n')
printf '%s\n' "$date_fmt_out" > "$WORK_DIR/date_fmt.out"
assert_file_contains "$WORK_DIR/date_fmt.out" '^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]$' "date +FORMAT output was not formatted correctly"
date_epoch_out=$("$ROOT_DIR/build/date" -d @0 +%Y-%m-%dT%H:%M:%S | tr -d '\r\n')
assert_text_equals "$date_epoch_out" '1970-01-01T00:00:00' "date -d @0 did not honor the requested epoch"
printf 'timestamp reference\n' > "$WORK_DIR/date_reference.txt"
touch -t 200001021234 "$WORK_DIR/date_reference.txt"
date_reference_out=$("$ROOT_DIR/build/date" -l -r "$WORK_DIR/date_reference.txt" +%Y-%m-%d | tr -d '\r\n')
assert_text_equals "$date_reference_out" '2000-01-02' "date -r did not use the file modification time"
uname_out=$("$ROOT_DIR/build/uname" -snrm | tr -d '\r\n')
case "$uname_out" in
    *" "*) ;;
    *) fail "uname flag output was unexpectedly thin" ;;
esac
uname_version_out=$("$ROOT_DIR/build/uname" --kernel-version | tr -d '\r\n')
[ -n "$uname_version_out" ] || fail "uname --kernel-version produced no output"

env_out=$(FOO=bar "$ROOT_DIR/build/env" | tr -d '\r')
case "$env_out" in
    *FOO=bar*) ;;
    *) fail "env output missing variable" ;;
esac
env_cmd_out=$("$ROOT_DIR/build/env" BAR=baz "$ROOT_DIR/build/sh" -c 'echo $BAR' | tr -d '\r\n')
assert_text_equals "$env_cmd_out" 'baz' "env command override failed"
env_empty_out=$(FOO=bar "$ROOT_DIR/build/env" -i BAR=baz "$ROOT_DIR/build/sh" -c 'echo $BAR' | tr -d '\r\n')
assert_text_equals "$env_empty_out" 'baz' "env -i did not create a clean environment for the command"
env_unset_out=$(FOO=bar "$ROOT_DIR/build/env" -u FOO "$ROOT_DIR/build/sh" -c 'echo $FOO' | tr -d '\r\n')
assert_text_equals "$env_unset_out" '' "env -u did not remove the requested variable"
env_clean_out=$("$ROOT_DIR/build/env" -i FOO=solo | tr '\0\r' '\n')
assert_text_equals "$env_clean_out" 'FOO=solo' "env -i did not reset the environment"
FOO=gone "$ROOT_DIR/build/env" -u FOO > "$WORK_DIR/env_unset.out"
if grep '^FOO=' "$WORK_DIR/env_unset.out" >/dev/null 2>&1; then
    fail "env -u did not remove the variable"
fi
env_zero_out=$("$ROOT_DIR/build/env" -0 FOO=bar | tr '\0' '\n' | grep '^FOO=' | tr -d '\r\n')
assert_text_equals "$env_zero_out" 'FOO=bar' "env -0 did not emit NUL-delimited output"

hostname_out=$("$ROOT_DIR/build/hostname" | tr -d '\r\n')
[ -n "$hostname_out" ] || fail "hostname output was empty"

sleep_multi_start=$(date +%s)
assert_command_succeeds "$ROOT_DIR/build/sleep" 0.01 0.02s
sleep_multi_end=$(date +%s)
[ "$sleep_multi_end" -ge "$sleep_multi_start" ] || fail "sleep duration parsing regressed"

printf 'hello tee\n' | "$ROOT_DIR/build/tee" "$WORK_DIR/tee.txt" > "$WORK_DIR/tee.out"
assert_files_equal "$WORK_DIR/tee.txt" "$WORK_DIR/tee.out" "tee output mismatch"
printf 'first\n' > "$WORK_DIR/tee_append.txt"
printf 'second\n' | "$ROOT_DIR/build/tee" -a "$WORK_DIR/tee_append.txt" > /dev/null
cat > "$WORK_DIR/tee_append.expected" <<'EOF'
first
second
EOF
assert_files_equal "$WORK_DIR/tee_append.expected" "$WORK_DIR/tee_append.txt" "tee -a did not append to the existing file"

printf 'one two\n' | "$ROOT_DIR/build/xargs" "$ROOT_DIR/build/echo" prefix > "$WORK_DIR/xargs.out"
actual_xargs=$(tr -d '\r' < "$WORK_DIR/xargs.out" | head -n 1)
assert_text_equals "$actual_xargs" 'prefix one two' "xargs argument composition failed"
printf 'a b c d\n' | "$ROOT_DIR/build/xargs" -n 2 "$ROOT_DIR/build/echo" batch > "$WORK_DIR/xargs_n.out"
cat > "$WORK_DIR/xargs_n.expected" <<'EOF'
batch a b
batch c d
EOF
assert_files_equal "$WORK_DIR/xargs_n.expected" "$WORK_DIR/xargs_n.out" "xargs -n batching failed"
printf 'left side\nnext item\n' | "$ROOT_DIR/build/xargs" -I '{}' "$ROOT_DIR/build/echo" 'pre:{}:post' > "$WORK_DIR/xargs_i.out"
cat > "$WORK_DIR/xargs_i.expected" <<'EOF'
pre:left side:post
pre:next item:post
EOF
assert_files_equal "$WORK_DIR/xargs_i.expected" "$WORK_DIR/xargs_i.out" "xargs -I replacement failed"
printf 'red\0blue sky\0' | "$ROOT_DIR/build/xargs" -0 -n 1 "$ROOT_DIR/build/echo" item > "$WORK_DIR/xargs_0.out"
cat > "$WORK_DIR/xargs_0.expected" <<'EOF'
item red
item blue sky
EOF
assert_files_equal "$WORK_DIR/xargs_0.expected" "$WORK_DIR/xargs_0.out" "xargs -0 parsing failed"
printf 'copy-a\n' > "$WORK_DIR/cp_a.txt"
printf 'copy-b\n' > "$WORK_DIR/cp_b.txt"
mkdir -p "$WORK_DIR/cp_dest"
assert_command_succeeds "$ROOT_DIR/build/cp" "$WORK_DIR/cp_a.txt" "$WORK_DIR/cp_b.txt" "$WORK_DIR/cp_dest"
assert_file_contains "$WORK_DIR/cp_dest/cp_a.txt" 'copy-a' "cp multi-source copy failed for first file"
assert_file_contains "$WORK_DIR/cp_dest/cp_b.txt" 'copy-b' "cp multi-source copy failed for second file"
printf 'keep-me\n' > "$WORK_DIR/cp_keep.txt"
printf 'replace-me\n' > "$WORK_DIR/cp_replace.txt"
assert_command_succeeds "$ROOT_DIR/build/cp" -n "$WORK_DIR/cp_replace.txt" "$WORK_DIR/cp_keep.txt"
assert_file_contains "$WORK_DIR/cp_keep.txt" '^keep-me$' "cp -n overwrote an existing destination"
printf 'older-source\n' > "$WORK_DIR/cp_update_src.txt"
"$ROOT_DIR/build/sleep" 1
printf 'newer-destination\n' > "$WORK_DIR/cp_update_dest.txt"
assert_command_succeeds "$ROOT_DIR/build/cp" -u "$WORK_DIR/cp_update_src.txt" "$WORK_DIR/cp_update_dest.txt"
assert_file_contains "$WORK_DIR/cp_update_dest.txt" '^newer-destination$' "cp -u replaced a newer destination"
"$ROOT_DIR/build/sleep" 1
printf 'fresh-source\n' > "$WORK_DIR/cp_update_src.txt"
assert_command_succeeds "$ROOT_DIR/build/cp" -u "$WORK_DIR/cp_update_src.txt" "$WORK_DIR/cp_update_dest.txt"
assert_file_contains "$WORK_DIR/cp_update_dest.txt" '^fresh-source$' "cp -u did not refresh an older destination"
printf 'echo archive\n' > "$WORK_DIR/archive-target.txt"
ln -sf archive-target.txt "$WORK_DIR/archive-link"
mkdir -p "$WORK_DIR/archive-dest"
assert_command_succeeds "$ROOT_DIR/build/cp" -a "$WORK_DIR/archive-link" "$WORK_DIR/archive-dest"
[ -L "$WORK_DIR/archive-dest/archive-link" ] || fail "cp -a did not preserve the symlink itself"
archive_link_out=$("$ROOT_DIR/build/readlink" "$WORK_DIR/archive-dest/archive-link" | tr -d '\r\n')
assert_text_equals "$archive_link_out" 'archive-target.txt' "cp -a preserved the wrong symlink target"
printf 'move-a\n' > "$WORK_DIR/mv_a.txt"
printf 'move-b\n' > "$WORK_DIR/mv_b.txt"
mkdir -p "$WORK_DIR/mv_dest"
assert_command_succeeds "$ROOT_DIR/build/mv" "$WORK_DIR/mv_a.txt" "$WORK_DIR/mv_b.txt" "$WORK_DIR/mv_dest"
assert_file_contains "$WORK_DIR/mv_dest/mv_a.txt" 'move-a' "mv multi-source move failed for first file"
assert_file_contains "$WORK_DIR/mv_dest/mv_b.txt" 'move-b' "mv multi-source move failed for second file"
[ ! -e "$WORK_DIR/mv_a.txt" ] || fail "mv multi-source move left the original file behind"
[ ! -e "$WORK_DIR/mv_b.txt" ] || fail "mv multi-source move left the second original file behind"
printf 'older-source\n' > "$WORK_DIR/mv_update_src.txt"
"$ROOT_DIR/build/sleep" 1
printf 'newer-destination\n' > "$WORK_DIR/mv_update_dest.txt"
assert_command_succeeds "$ROOT_DIR/build/mv" -u "$WORK_DIR/mv_update_src.txt" "$WORK_DIR/mv_update_dest.txt"
[ -f "$WORK_DIR/mv_update_src.txt" ] || fail "mv -u should keep the source when the destination is newer"
assert_file_contains "$WORK_DIR/mv_update_dest.txt" '^newer-destination$' "mv -u overwrote a newer destination"
"$ROOT_DIR/build/sleep" 1
printf 'fresh-source\n' > "$WORK_DIR/mv_update_src.txt"
"$ROOT_DIR/build/mv" -uv "$WORK_DIR/mv_update_src.txt" "$WORK_DIR/mv_update_dest.txt" > "$WORK_DIR/mv_uv.out"
assert_file_contains "$WORK_DIR/mv_uv.out" 'mv_update_src.txt -> .*mv_update_dest.txt' "mv -v did not report the move"
[ ! -e "$WORK_DIR/mv_update_src.txt" ] || fail "mv -u/-v left the source file behind"
assert_file_contains "$WORK_DIR/mv_update_dest.txt" '^fresh-source$' "mv -u did not replace an older destination"

printf 'abcdef' > "$WORK_DIR/dd.in"
"$ROOT_DIR/build/dd" if="$WORK_DIR/dd.in" of="$WORK_DIR/dd.out" bs=2 count=2
printf 'abcd' > "$WORK_DIR/dd.expected"
assert_files_equal "$WORK_DIR/dd.out" "$WORK_DIR/dd.expected" "dd block copy failed"
printf 'xyz' > "$WORK_DIR/dd_sync.in"
assert_command_succeeds "$ROOT_DIR/build/dd" if="$WORK_DIR/dd_sync.in" of="$WORK_DIR/dd_sync.out" bs=4 count=1 conv=sync status=none
dd_sync_size=$(wc -c < "$WORK_DIR/dd_sync.out" | tr -d ' \r\n')
assert_text_equals "$dd_sync_size" '4' "dd conv=sync did not pad the short block to the requested size"
dd_sync_prefix=$("$ROOT_DIR/build/head" -c 3 "$WORK_DIR/dd_sync.out" | tr -d '\r\n')
assert_text_equals "$dd_sync_prefix" 'xyz' "dd conv=sync altered the original payload"
printf '01abcdef' > "$WORK_DIR/dd_notrunc.out"
printf 'XY' > "$WORK_DIR/dd_patch.in"
assert_command_succeeds "$ROOT_DIR/build/dd" if="$WORK_DIR/dd_patch.in" of="$WORK_DIR/dd_notrunc.out" bs=2 count=1 seek=1 conv=notrunc status=none
dd_notrunc_out=$("$ROOT_DIR/build/head" -c 8 "$WORK_DIR/dd_notrunc.out" | tr -d '\r\n')
assert_text_equals "$dd_notrunc_out" '01XYcdef' "dd conv=notrunc should preserve the surrounding destination bytes"
"$ROOT_DIR/build/dd" if="$WORK_DIR/dd.in" of="$WORK_DIR/dd_status.out" bs=2 count=1 status=noxfer 2> "$WORK_DIR/dd_status.err"
assert_file_contains "$WORK_DIR/dd_status.err" 'records in' "dd status=noxfer did not emit record counters"
if grep 'bytes copied' "$WORK_DIR/dd_status.err" >/dev/null 2>&1; then
    fail "dd status=noxfer should suppress the byte summary"
fi

seq_decimal_out=$("$ROOT_DIR/build/seq" -w 0.5 0.25 1.0 | tr -d '\r')
printf '%s\n' "$seq_decimal_out" > "$WORK_DIR/seq_decimal.out"
cat > "$WORK_DIR/seq_decimal.expected" <<'EOF'
0.50
0.75
1.00
EOF
assert_files_equal "$WORK_DIR/seq_decimal.expected" "$WORK_DIR/seq_decimal.out" "seq decimal stepping or -w formatting failed"
seq_format_out=$("$ROOT_DIR/build/seq" -s ',' -f 'item%.1f' 1 0.5 2 | tr -d '\r\n')
assert_text_equals "$seq_format_out" 'item1.0,item1.5,item2.0' "seq -f or separator handling failed"

printf 'truncate\n' > "$WORK_DIR/truncate_io.txt"
assert_command_succeeds "$ROOT_DIR/build/truncate" -o -s 1 "$WORK_DIR/truncate_io.txt"
truncate_io_size=$(wc -c < "$WORK_DIR/truncate_io.txt" | tr -d ' \r\n')
assert_text_equals "$truncate_io_size" '512' "truncate -o should treat the size as I/O blocks"
assert_command_succeeds "$ROOT_DIR/build/truncate" -c -s 5 "$WORK_DIR/truncate_missing.txt"
[ ! -e "$WORK_DIR/truncate_missing.txt" ] || fail "truncate -c should not create a missing file"

assert_command_succeeds "$ROOT_DIR/build/sync" -d -v "$WORK_DIR/truncate_io.txt" > "$WORK_DIR/sync_v.out"
assert_file_contains "$WORK_DIR/sync_v.out" 'synced .*truncate_io.txt' "sync -v did not report the synced path"

"$ROOT_DIR/build/od" "$WORK_DIR/dd.in" > "$WORK_DIR/od.out"
assert_file_contains "$WORK_DIR/od.out" '141 142 143 144' "od dump missing expected bytes"

"$ROOT_DIR/build/hexdump" "$WORK_DIR/dd.in" > "$WORK_DIR/hexdump.out"
assert_file_contains "$WORK_DIR/hexdump.out" '61 62 63 64' "hexdump missing expected hex bytes"

actual_basename=$("$ROOT_DIR/build/basename" /tmp/example.txt .txt | tr -d '\r\n')
assert_text_equals "$actual_basename" 'example' "basename failed"
basename_multi=$("$ROOT_DIR/build/basename" -a -s .txt "$WORK_DIR/alpha.txt" "$WORK_DIR/beta.txt" | tr -d '\r')
printf '%s\n' "$basename_multi" > "$WORK_DIR/basename_multi.out"
assert_file_contains "$WORK_DIR/basename_multi.out" '^alpha$' "basename -a/-s did not strip the suffix from the first path"
assert_file_contains "$WORK_DIR/basename_multi.out" '^beta$' "basename -a/-s did not strip the suffix from the second path"
actual_dirname=$("$ROOT_DIR/build/dirname" /tmp/example.txt | tr -d '\r\n')
assert_text_equals "$actual_dirname" '/tmp' "dirname failed"
dirname_multi=$("$ROOT_DIR/build/dirname" "$WORK_DIR/sub/example.txt" "$WORK_DIR/dd.in" | tr -d '\r')
printf '%s\n' "$dirname_multi" > "$WORK_DIR/dirname_multi.out"
assert_file_contains "$WORK_DIR/dirname_multi.out" '^.*/sub$' "dirname multi-path mode missed the nested directory"
actual_realpath=$("$ROOT_DIR/build/realpath" "$WORK_DIR/sub/../dd.in" | tr -d '\r\n')
assert_text_equals "$actual_realpath" "$WORK_DIR/dd.in" "realpath normalization failed"
ln -sf ../dd.in "$WORK_DIR/sub/link-dd"
realpath_link=$("$ROOT_DIR/build/realpath" "$WORK_DIR/sub/link-dd" | tr -d '\r\n')
assert_text_equals "$realpath_link" "$WORK_DIR/dd.in" "realpath did not resolve the symlink target"
realpath_missing=$("$ROOT_DIR/build/realpath" -m "$WORK_DIR/sub/missing/../ghost.txt" | tr -d '\r\n')
assert_text_equals "$realpath_missing" "$WORK_DIR/sub/ghost.txt" "realpath -m did not normalize a missing path"
mkdir -p "$WORK_DIR/realpath_policy/base/real/sub"
ln -sf real/sub "$WORK_DIR/realpath_policy/base/linksub"
realpath_physical_policy=$("$ROOT_DIR/build/realpath" -P "$WORK_DIR/realpath_policy/base/linksub/.." | tr -d '\r\n')
assert_text_equals "$realpath_physical_policy" "$WORK_DIR/realpath_policy/base/real" "realpath -P should resolve symlinks before processing .."
realpath_logical_policy=$("$ROOT_DIR/build/realpath" -L "$WORK_DIR/realpath_policy/base/linksub/.." | tr -d '\r\n')
assert_text_equals "$realpath_logical_policy" "$WORK_DIR/realpath_policy/base" "realpath -L should honor logical traversal through .."
pwd_logical=$(
    cd "$WORK_DIR/sub"
    PWD="$WORK_DIR/sub" "$ROOT_DIR/build/pwd" -L | tr -d '\r\n'
)
assert_text_equals "$pwd_logical" "$WORK_DIR/sub" "pwd -L did not honor a valid logical path"
pwd_physical=$(
    cd "$WORK_DIR/sub"
    PWD="$WORK_DIR" "$ROOT_DIR/build/pwd" -P | tr -d '\r\n'
)
assert_text_equals "$pwd_physical" "$WORK_DIR/sub" "pwd -P did not print the physical working directory"

mkdir -p "$WORK_DIR/find_tree/keep" "$WORK_DIR/find_tree/skip/deeper"
printf 'keep\n' > "$WORK_DIR/find_tree/keep/kept.txt"
printf 'skip\n' > "$WORK_DIR/find_tree/skip/deeper/skipped.txt"
ln -sf keep/kept.txt "$WORK_DIR/find_tree/kept.link"
"$ROOT_DIR/build/find" "$WORK_DIR/find_tree" -maxdepth 1 > "$WORK_DIR/find_maxdepth.out"
assert_file_contains "$WORK_DIR/find_maxdepth.out" 'find_tree$' "find -maxdepth did not include the starting directory"
if grep 'kept.txt\|skipped.txt' "$WORK_DIR/find_maxdepth.out" >/dev/null 2>&1; then
    fail "find -maxdepth descended too deeply"
fi
"$ROOT_DIR/build/find" "$WORK_DIR/find_tree" -mindepth 1 -type f -print0 | tr '\0' '\n' > "$WORK_DIR/find_print0.out"
assert_file_contains "$WORK_DIR/find_print0.out" 'kept.txt' "find -mindepth/-print0 missed the shallow file"
assert_file_contains "$WORK_DIR/find_print0.out" 'skipped.txt' "find -mindepth/-print0 missed the nested file"
"$ROOT_DIR/build/find" "$WORK_DIR/find_tree" -type l > "$WORK_DIR/find_links.out"
assert_file_contains "$WORK_DIR/find_links.out" 'kept.link' "find -type l did not report symlinks"
"$ROOT_DIR/build/find" "$WORK_DIR/find_tree" -name kept.txt -exec "$ROOT_DIR/build/echo" found {} ';' > "$WORK_DIR/find_exec.out"
assert_file_contains "$WORK_DIR/find_exec.out" 'found .*kept.txt' "find -exec did not execute the requested command"

printf 'older\n' > "$WORK_DIR/test_old.txt"
"$ROOT_DIR/build/sleep" 1
printf 'newer\n' > "$WORK_DIR/test_new.txt"
assert_command_succeeds "$ROOT_DIR/build/test" "$WORK_DIR/test_new.txt" -nt "$WORK_DIR/test_old.txt"
if "$ROOT_DIR/build/test" "$WORK_DIR/test_old.txt" -nt "$WORK_DIR/test_new.txt"; then
    fail "test -nt reported the wrong ordering"
fi
ln "$WORK_DIR/test_new.txt" "$WORK_DIR/test_same.txt"
assert_command_succeeds "$ROOT_DIR/build/test" "$WORK_DIR/test_new.txt" -ef "$WORK_DIR/test_same.txt"
ln -sf test_new.txt "$WORK_DIR/test_link"
assert_command_succeeds "$ROOT_DIR/build/test" "$WORK_DIR/test_new.txt" -ef "$WORK_DIR/test_link"
assert_command_succeeds "$ROOT_DIR/build/[" -L "$WORK_DIR/test_link" ]
assert_command_succeeds "$ROOT_DIR/build/test" alpha '<' beta
if "$ROOT_DIR/build/test" gamma '<' beta; then
    fail "test string comparison unexpectedly succeeded"
fi

assert_command_succeeds "$ROOT_DIR/build/cmp" "$WORK_DIR/dd.in" "$WORK_DIR/dd.in"
printf 'left\nline2\n' > "$WORK_DIR/left.txt"
printf 'left\nchanged\n' > "$WORK_DIR/right.txt"
if "$ROOT_DIR/build/cmp" -s "$WORK_DIR/left.txt" "$WORK_DIR/right.txt"; then
    fail "cmp -s should have reported a difference"
fi
cmp_list_out=$("$ROOT_DIR/build/cmp" -l "$WORK_DIR/left.txt" "$WORK_DIR/right.txt" | tr -d '\r')
printf '%s\n' "$cmp_list_out" > "$WORK_DIR/cmp_l.out"
assert_file_contains "$WORK_DIR/cmp_l.out" '^[0-9][0-9]* ' "cmp -l did not report the differing byte positions"
printf 'xxabcdefzz' > "$WORK_DIR/cmp_skip_left.txt"
printf 'yyabcdefqq' > "$WORK_DIR/cmp_skip_right.txt"
assert_command_succeeds "$ROOT_DIR/build/cmp" -i 2:2 -n 6 "$WORK_DIR/cmp_skip_left.txt" "$WORK_DIR/cmp_skip_right.txt"
if "$ROOT_DIR/build/cmp" -i 2:2 -n 8 "$WORK_DIR/cmp_skip_left.txt" "$WORK_DIR/cmp_skip_right.txt" >/dev/null 2>&1; then
    fail "cmp -i/-n should have detected the later differing bytes"
fi
if "$ROOT_DIR/build/diff" "$WORK_DIR/left.txt" "$WORK_DIR/right.txt" > "$WORK_DIR/diff.out"; then
    fail "diff should have reported a difference"
fi
assert_file_contains "$WORK_DIR/diff.out" '^line 2:$' "diff output missing changed line"
if "$ROOT_DIR/build/diff" -u "$WORK_DIR/left.txt" "$WORK_DIR/right.txt" > "$WORK_DIR/diff_u.out"; then
    fail "diff -u should have reported a difference"
fi
assert_file_contains "$WORK_DIR/diff_u.out" '^--- ' "diff -u header missing"
assert_file_contains "$WORK_DIR/diff_u.out" '^@@ ' "diff -u hunk header missing"
mkdir -p "$WORK_DIR/diff_left/sub" "$WORK_DIR/diff_right/sub"
printf 'only-left\n' > "$WORK_DIR/diff_left/only.txt"
printf 'same\n' > "$WORK_DIR/diff_left/sub/common.txt"
printf 'changed-right\n' > "$WORK_DIR/diff_right/sub/common.txt"
if "$ROOT_DIR/build/diff" -qr "$WORK_DIR/diff_left" "$WORK_DIR/diff_right" > "$WORK_DIR/diff_qr.out"; then
    fail "diff -qr should have reported directory differences"
fi
assert_file_contains "$WORK_DIR/diff_qr.out" 'Only in .*diff_left: only.txt' "diff -qr did not report one-sided files"
assert_file_contains "$WORK_DIR/diff_qr.out" 'Files .*common.txt.* differ' "diff -qr did not report nested file changes"

"$ROOT_DIR/build/file" "$WORK_DIR/left.txt" > "$WORK_DIR/file.out"
assert_file_contains "$WORK_DIR/file.out" 'ASCII text' "file type detection failed"
ln -sf left.txt "$WORK_DIR/file-link"
"$ROOT_DIR/build/file" "$WORK_DIR/file-link" > "$WORK_DIR/file_link.out"
assert_file_contains "$WORK_DIR/file_link.out" 'symbolic link to left.txt' "file should describe symlinks by default"
"$ROOT_DIR/build/file" -L "$WORK_DIR/file-link" > "$WORK_DIR/file_follow.out"
assert_file_contains "$WORK_DIR/file_follow.out" 'ASCII text' "file -L did not inspect the symlink target"
printf '\211PNG\r\n\032\nrest' > "$WORK_DIR/sample.png"
file_mime_out=$("$ROOT_DIR/build/file" -i "$WORK_DIR/sample.png" | tr -d '\r\n')
assert_text_equals "$file_mime_out" "$WORK_DIR/sample.png: image/png" "file -i did not report the PNG MIME type"

printf '\001\002HelloString\000xxMoreText\n' > "$WORK_DIR/strings.bin"
"$ROOT_DIR/build/strings" "$WORK_DIR/strings.bin" > "$WORK_DIR/strings.out"
assert_file_contains "$WORK_DIR/strings.out" 'HelloString' "strings output missing printable sequence"
"$ROOT_DIR/build/strings" -t x "$WORK_DIR/strings.bin" > "$WORK_DIR/strings_offset.out"
assert_file_contains "$WORK_DIR/strings_offset.out" '^2 HelloString$' "strings -t x did not include the expected offset"

printf_out=$("$ROOT_DIR/build/printf" 'value=%s %d %x\n' sample 42 255 | tr -d '\r')
assert_text_equals "$printf_out" 'value=sample 42 ff' "printf formatting failed"
"$ROOT_DIR/build/printf" '%.3s %b' sample 'A\tB' > "$WORK_DIR/printf_features.out"
printf 'sam A\tB' > "$WORK_DIR/printf_features.expected"
assert_files_equal "$WORK_DIR/printf_features.expected" "$WORK_DIR/printf_features.out" "printf precision or %b handling failed"
printf_cycle=$("$ROOT_DIR/build/printf" '%s:' A B C | tr -d '\r\n')
assert_text_equals "$printf_cycle" 'A:B:C:' "printf format cycling failed"
echo_out=$("$ROOT_DIR/build/echo" -n sample | tr -d '\r')
assert_text_equals "$echo_out" 'sample' "echo -n failed"
"$ROOT_DIR/build/echo" -e 'line1\nline2\tend' > "$WORK_DIR/echo_escape.out"
printf 'line1\nline2\tend\n' > "$WORK_DIR/echo_escape.expected"
assert_files_equal "$WORK_DIR/echo_escape.expected" "$WORK_DIR/echo_escape.out" "echo -e did not decode common escapes"
echo_literal_out=$("$ROOT_DIR/build/echo" -E 'left\nright' | tr -d '\r\n')
assert_text_equals "$echo_literal_out" 'left\nright' "echo -E should leave backslash sequences untouched"
echo_stop_out=$("$ROOT_DIR/build/echo" -e 'done\cignored' tail | tr -d '\r')
assert_text_equals "$echo_stop_out" 'done' "echo -e \\c did not stop output early"
printf 'alpha\n\n\nbeta\n' > "$WORK_DIR/cat_flags.txt"
"$ROOT_DIR/build/cat" -n "$WORK_DIR/cat_flags.txt" > "$WORK_DIR/cat_n.out"
assert_file_contains "$WORK_DIR/cat_n.out" '^[[:space:]]*1[[:space:]]alpha$' "cat -n did not number the first line"
"$ROOT_DIR/build/cat" -b "$WORK_DIR/cat_flags.txt" > "$WORK_DIR/cat_b.out"
assert_file_contains "$WORK_DIR/cat_b.out" '^[[:space:]]*2[[:space:]]beta$' "cat -b did not skip blank lines when numbering"
"$ROOT_DIR/build/cat" -s "$WORK_DIR/cat_flags.txt" > "$WORK_DIR/cat_s.out"
cat > "$WORK_DIR/cat_s.expected" <<'EOF'
alpha

beta
EOF
assert_files_equal "$WORK_DIR/cat_s.expected" "$WORK_DIR/cat_s.out" "cat -s did not squeeze repeated blank lines"

which_out=$("$ROOT_DIR/build/which" ls | tr -d '\r\n')
assert_text_equals "$which_out" "$ROOT_DIR/build/ls" "which did not resolve build tool"
"$ROOT_DIR/build/which" -a ls > "$WORK_DIR/which_all.out"
assert_file_contains "$WORK_DIR/which_all.out" 'build/ls' "which -a did not report all matches"
mkdir -p "$WORK_DIR/path1" "$WORK_DIR/path2"
printf '#!/bin/sh\necho one\n' > "$WORK_DIR/path1/dupcmd"
printf '#!/bin/sh\necho two\n' > "$WORK_DIR/path2/dupcmd"
chmod +x "$WORK_DIR/path1/dupcmd" "$WORK_DIR/path2/dupcmd"
which_all_out=$(PATH="$WORK_DIR/path1:$WORK_DIR/path2:$PATH" "$ROOT_DIR/build/which" -a dupcmd | tr -d '\r')
assert_file_contains "$WORK_DIR/left.txt" '^left$' "diff fixture missing"
printf '%s\n' "$which_all_out" > "$WORK_DIR/which_all.out"
assert_file_contains "$WORK_DIR/which_all.out" 'path1/dupcmd' "which -a missing first match"
assert_file_contains "$WORK_DIR/which_all.out" 'path2/dupcmd' "which -a missing second match"

printf 'Hello\nNope\nHello\n' > "$WORK_DIR/grep_count.txt"
grep_count=$("$ROOT_DIR/build/grep" -c Hello "$WORK_DIR/grep_count.txt" | tr -d '\r\n')
assert_text_equals "$grep_count" '2' "grep count mode failed"
if ! "$ROOT_DIR/build/grep" -q Hello "$WORK_DIR/grep_count.txt"; then
    fail "grep quiet mode failed"
fi
"$ROOT_DIR/build/grep" -l Hello "$WORK_DIR/grep_count.txt" > "$WORK_DIR/grep_l.out"
assert_file_contains "$WORK_DIR/grep_l.out" 'grep_count.txt' "grep list-files mode failed"
grep_fixed=$("$ROOT_DIR/build/grep" -F 'Hell' "$WORK_DIR/grep_count.txt" | tr -d '\r')
case "$grep_fixed" in
    *Hello*) ;;
    *) fail "grep fixed-string mode failed" ;;
esac
"$ROOT_DIR/build/grep" -o 'H.*o' "$WORK_DIR/grep_count.txt" > "$WORK_DIR/grep_o.out"
assert_file_contains "$WORK_DIR/grep_o.out" '^Hello$' "grep only-matching mode failed"
printf 'foo.bar\nfooXbar\nfoo.bar baz\n' > "$WORK_DIR/grep_fixed.txt"
"$ROOT_DIR/build/grep" -Fo 'foo.bar' "$WORK_DIR/grep_fixed.txt" > "$WORK_DIR/grep_fo.out"
cat > "$WORK_DIR/grep_fo.expected" <<'EOF'
foo.bar
foo.bar
EOF
assert_files_equal "$WORK_DIR/grep_fo.expected" "$WORK_DIR/grep_fo.out" "grep -F/-o literal matching failed"
printf 'word\nsword\nword.\nwordplay\n' > "$WORK_DIR/grep_word.txt"
"$ROOT_DIR/build/grep" -w word "$WORK_DIR/grep_word.txt" > "$WORK_DIR/grep_word.out"
cat > "$WORK_DIR/grep_word.expected" <<'EOF'
word
word.
EOF
assert_files_equal "$WORK_DIR/grep_word.expected" "$WORK_DIR/grep_word.out" "grep whole-word mode failed"
printf 'zero\none\ntwo\nthree\nfour\n' > "$WORK_DIR/grep_context.txt"
"$ROOT_DIR/build/grep" -n -C 1 two "$WORK_DIR/grep_context.txt" > "$WORK_DIR/grep_context.out"
cat > "$WORK_DIR/grep_context.expected" <<'EOF'
2-one
3:two
4-three
EOF
assert_files_equal "$WORK_DIR/grep_context.expected" "$WORK_DIR/grep_context.out" "grep context mode failed"

wc_selected=$(printf 'one two\nthree\n' | "$ROOT_DIR/build/wc" -lw | tr -d '\r\n')
assert_text_equals "$wc_selected" '2 3' "wc selective counts failed"

printf 'abcdef12345' > "$WORK_DIR/bytes.txt"
head_bytes=$("$ROOT_DIR/build/head" -c 5 "$WORK_DIR/bytes.txt" | tr -d '\r\n')
assert_text_equals "$head_bytes" 'abcde' "head byte mode failed"
tail_bytes=$("$ROOT_DIR/build/tail" -c 5 "$WORK_DIR/bytes.txt" | tr -d '\r\n')
assert_text_equals "$tail_bytes" '12345' "tail byte mode failed"
printf 'one\n' > "$WORK_DIR/head_one.txt"
printf 'two\n' > "$WORK_DIR/head_two.txt"
"$ROOT_DIR/build/head" -q "$WORK_DIR/head_one.txt" "$WORK_DIR/head_two.txt" > "$WORK_DIR/head_q.out"
if grep '^==>' "$WORK_DIR/head_q.out" >/dev/null 2>&1; then
    fail "head -q should suppress file headers"
fi
"$ROOT_DIR/build/tail" -v "$WORK_DIR/head_one.txt" > "$WORK_DIR/tail_v.out"
assert_file_contains "$WORK_DIR/tail_v.out" '^==> .*head_one.txt <==$' "tail -v should force a header"
printf 'first\nsecond\nthird\nfourth\n' > "$WORK_DIR/head_tail_compat.txt"
"$ROOT_DIR/build/head" -n +3 "$WORK_DIR/head_tail_compat.txt" > "$WORK_DIR/head_plus.out"
cat > "$WORK_DIR/head_plus.expected" <<'EOF'
third
fourth
EOF
assert_files_equal "$WORK_DIR/head_plus.expected" "$WORK_DIR/head_plus.out" "head -n +N should start at the requested line"
"$ROOT_DIR/build/head" -n -1 "$WORK_DIR/head_tail_compat.txt" > "$WORK_DIR/head_minus.out"
cat > "$WORK_DIR/head_minus.expected" <<'EOF'
first
second
third
EOF
assert_files_equal "$WORK_DIR/head_minus.expected" "$WORK_DIR/head_minus.out" "head -n -N should omit the last lines"
"$ROOT_DIR/build/head" -2 "$WORK_DIR/head_tail_compat.txt" > "$WORK_DIR/head_legacy.out"
cat > "$WORK_DIR/head_legacy.expected" <<'EOF'
first
second
EOF
assert_files_equal "$WORK_DIR/head_legacy.expected" "$WORK_DIR/head_legacy.out" "head legacy -NUM form failed"
"$ROOT_DIR/build/tail" -n +3 "$WORK_DIR/head_tail_compat.txt" > "$WORK_DIR/tail_plus.out"
assert_files_equal "$WORK_DIR/head_plus.expected" "$WORK_DIR/tail_plus.out" "tail -n +N should start at the requested line"
"$ROOT_DIR/build/tail" -2 "$WORK_DIR/head_tail_compat.txt" > "$WORK_DIR/tail_legacy.out"
cat > "$WORK_DIR/tail_legacy.expected" <<'EOF'
third
fourth
EOF
assert_files_equal "$WORK_DIR/tail_legacy.expected" "$WORK_DIR/tail_legacy.out" "tail legacy -NUM form failed"
head_byte_plus=$("$ROOT_DIR/build/head" -c +3 "$WORK_DIR/bytes.txt" | tr -d '\r\n')
assert_text_equals "$head_byte_plus" 'cdef12345' "head -c +N should start at the requested byte"
head_byte_minus=$("$ROOT_DIR/build/head" -c -5 "$WORK_DIR/bytes.txt" | tr -d '\r\n')
assert_text_equals "$head_byte_minus" 'abcdef' "head -c -N should omit the final bytes"
tail_byte_plus=$("$ROOT_DIR/build/tail" -c +7 "$WORK_DIR/bytes.txt" | tr -d '\r\n')
assert_text_equals "$tail_byte_plus" '12345' "tail -c +N should start at the requested byte"

printf 'name:role:team\nAda:Eng:Kernel\nBob:Ops:Infra\n' > "$WORK_DIR/cut_fields.txt"
"$ROOT_DIR/build/cut" -d ':' -f 1,3 "$WORK_DIR/cut_fields.txt" > "$WORK_DIR/cut_fields.out"
cat > "$WORK_DIR/cut_fields.expected" <<'EOF'
name:team
Ada:Kernel
Bob:Infra
EOF
assert_files_equal "$WORK_DIR/cut_fields.expected" "$WORK_DIR/cut_fields.out" "cut field mode failed"
cut_bytes_out=$("$ROOT_DIR/build/cut" -b 2-4 "$WORK_DIR/bytes.txt" | tr -d '\r\n')
assert_text_equals "$cut_bytes_out" 'bcd' "cut byte mode failed"
"$ROOT_DIR/build/cut" --complement -d ':' -f 2 "$WORK_DIR/cut_fields.txt" > "$WORK_DIR/cut_complement.out"
assert_files_equal "$WORK_DIR/cut_fields.expected" "$WORK_DIR/cut_complement.out" "cut complement mode failed"

tr_delete=$(printf 'a1b22c333\n' | "$ROOT_DIR/build/tr" -d '0-9' | tr -d '\r\n')
assert_text_equals "$tr_delete" 'abc' "tr delete range failed"
tr_squeeze=$(printf 'aa   bb    cc\n' | "$ROOT_DIR/build/tr" -s ' ' | tr -d '\r\n')
assert_text_equals "$tr_squeeze" 'aa bb cc' "tr squeeze mode failed"
tr_keep_digits=$(printf 'room 101b' | "$ROOT_DIR/build/tr" -cd '0-9' | tr -d '\r\n')
assert_text_equals "$tr_keep_digits" '101' "tr complement delete failed"
tr_mask_out=$(printf 'abc123' | "$ROOT_DIR/build/tr" -c '0-9' x | tr -d '\r\n')
assert_text_equals "$tr_mask_out" 'xxx123' "tr complement translate failed"
printf 'tag Alpha\nTAG alpha\nkeep beta\nKEEP beta\n' > "$WORK_DIR/uniq.txt"
"$ROOT_DIR/build/uniq" -i -f 1 -w 5 "$WORK_DIR/uniq.txt" > "$WORK_DIR/uniq.out"
cat > "$WORK_DIR/uniq.expected" <<'EOF'
tag Alpha
keep beta
EOF
assert_files_equal "$WORK_DIR/uniq.expected" "$WORK_DIR/uniq.out" "uniq comparison controls failed"
printf 'Alpha\0Alpha\0beta\0' > "$WORK_DIR/uniq_zero.txt"
"$ROOT_DIR/build/uniq" -z -c "$WORK_DIR/uniq_zero.txt" | tr '\0' '\n' > "$WORK_DIR/uniq_zero.out"
cat > "$WORK_DIR/uniq_zero.expected" <<'EOF'
2 Alpha
1 beta
EOF
assert_files_equal "$WORK_DIR/uniq_zero.expected" "$WORK_DIR/uniq_zero.out" "uniq -z did not preserve NUL-delimited groups"

printf 'keep\ndrop\nstop\nlater\n' > "$WORK_DIR/sed_more.txt"
"$ROOT_DIR/build/sed" '/drop/d' "$WORK_DIR/sed_more.txt" > "$WORK_DIR/sed_delete.out"
cat > "$WORK_DIR/sed_delete.expected" <<'EOF'
keep
stop
later
EOF
assert_files_equal "$WORK_DIR/sed_delete.expected" "$WORK_DIR/sed_delete.out" "sed delete command failed"
sed_print=$("$ROOT_DIR/build/sed" -n '2p;3q' "$WORK_DIR/sed_more.txt" | tr -d '\r\n')
assert_text_equals "$sed_print" 'drop' "sed -n with p/q commands failed"
printf 'pear:30\napple:10\nbanana:2\n' > "$WORK_DIR/sort_key.txt"
"$ROOT_DIR/build/sort" -t ':' -k 2,2 -n "$WORK_DIR/sort_key.txt" > "$WORK_DIR/sort_key.out"
cat > "$WORK_DIR/sort_key.expected" <<'EOF'
banana:2
apple:10
pear:30
EOF
assert_files_equal "$WORK_DIR/sort_key.expected" "$WORK_DIR/sort_key.out" "sort -k/-t numeric key selection failed"
printf 'alpha\nbeta\ngamma\n' > "$WORK_DIR/sed_insert.txt"
"$ROOT_DIR/build/sed" '1i HEADER;2c BETA;3a FOOTER' "$WORK_DIR/sed_insert.txt" > "$WORK_DIR/sed_insert.out"
cat > "$WORK_DIR/sed_insert.expected" <<'EOF'
HEADER
alpha
BETA
gamma
FOOTER
EOF
assert_files_equal "$WORK_DIR/sed_insert.expected" "$WORK_DIR/sed_insert.out" "sed insert/change/append commands failed"
printf 'red\nblue\n' > "$WORK_DIR/sed_inplace.txt"
assert_command_succeeds "$ROOT_DIR/build/sed" -i.bak '2c azure' "$WORK_DIR/sed_inplace.txt"
cat > "$WORK_DIR/sed_inplace.expected" <<'EOF'
red
azure
EOF
assert_files_equal "$WORK_DIR/sed_inplace.expected" "$WORK_DIR/sed_inplace.txt" "sed -i did not rewrite the file in place"
assert_file_contains "$WORK_DIR/sed_inplace.txt.bak" '^blue$' "sed -i backup suffix did not preserve the original content"
printf '\nfirst\n\nsecond\n' > "$WORK_DIR/nl_input.txt"
"$ROOT_DIR/build/nl" -ba -v 10 -i 5 -w 2 -s ': ' "$WORK_DIR/nl_input.txt" > "$WORK_DIR/nl_options.out"
printf '10: \n15: first\n20: \n25: second\n' > "$WORK_DIR/nl_options.expected"
assert_files_equal "$WORK_DIR/nl_options.expected" "$WORK_DIR/nl_options.out" "nl increment/separator controls failed"
printf 'abcdef' > "$WORK_DIR/split_input.txt"
(
    cd "$WORK_DIR"
    "$ROOT_DIR/build/split" -b 2 -a 3 -d split_input.txt part
)
assert_file_contains "$WORK_DIR/part000" '^ab$' "split -d/-a did not create the first numeric chunk"
assert_file_contains "$WORK_DIR/part001" '^cd$' "split -d/-a did not create the second numeric chunk"
assert_file_contains "$WORK_DIR/part002" '^ef$' "split -d/-a did not create the third numeric chunk"
printf 'head\nmid\none\nmid\ntwo\n' > "$WORK_DIR/csplit_input.txt"
(
    cd "$WORK_DIR"
    "$ROOT_DIR/build/csplit" -s -n 3 -f cut csplit_input.txt /mid/{1}
)
assert_file_contains "$WORK_DIR/cut000" '^head$' "csplit repeat-count split missed the leading segment"
assert_file_contains "$WORK_DIR/cut001" '^mid$' "csplit repeat-count split missed the first match segment"
assert_file_contains "$WORK_DIR/cut001" '^one$' "csplit repeat-count split omitted the body after the first match"
assert_file_contains "$WORK_DIR/cut002" '^mid$' "csplit repeat-count split missed the repeated match segment"
assert_file_contains "$WORK_DIR/cut002" '^two$' "csplit repeat-count split omitted the body after the repeated match"
(
    cd "$WORK_DIR"
    "$ROOT_DIR/build/csplit" -s -k -f keep csplit_input.txt /mid/ /missing/ >/dev/null 2>&1 || true
)
[ -f "$WORK_DIR/keep00" ] || fail "csplit -k should keep created output files after a later failure"

ln -sf dd.in "$WORK_DIR/link-to-dd"
readlink_out=$("$ROOT_DIR/build/readlink" "$WORK_DIR/link-to-dd" | tr -d '\r\n')
assert_text_equals "$readlink_out" 'dd.in' "readlink target mismatch"
readlink_full=$("$ROOT_DIR/build/readlink" -f "$WORK_DIR/sub/link-dd" | tr -d '\r\n')
assert_text_equals "$readlink_full" "$WORK_DIR/dd.in" "readlink -f did not canonicalize the symlink"
readlink_verbose=$("$ROOT_DIR/build/readlink" -v "$WORK_DIR/link-to-dd" | tr -d '\r\n')
assert_text_equals "$readlink_verbose" "$WORK_DIR/link-to-dd: dd.in" "readlink -v did not prefix the path"
"$ROOT_DIR/build/stat" "$WORK_DIR/dd.in" > "$WORK_DIR/stat.out"
assert_file_contains "$WORK_DIR/stat.out" '^Size:' "stat output missing size"
assert_file_contains "$WORK_DIR/stat.out" 'Type: file' "stat output missing file type"
assert_file_contains "$WORK_DIR/stat.out" '^Access:' "stat output missing access time"
assert_file_contains "$WORK_DIR/stat.out" '^Change:' "stat output missing change time"
stat_format_out=$("$ROOT_DIR/build/stat" -c '%F %a %n' "$WORK_DIR/dd.in" | tr -d '\r')
printf '%s\n' "$stat_format_out" > "$WORK_DIR/stat_format.out"
assert_file_contains "$WORK_DIR/stat_format.out" '^file [0-7][0-7][0-7][0-7]*[[:space:]].*dd\.in$' "stat -c format output was incomplete"
stat_follow_out=$("$ROOT_DIR/build/stat" -L -c '%F' "$WORK_DIR/sub/link-dd" | tr -d '\r\n')
assert_text_equals "$stat_follow_out" 'file' "stat -L did not follow the symlink"
stat_time_tokens=$("$ROOT_DIR/build/stat" -c '%X %Y %Z' "$WORK_DIR/dd.in" | tr -d '\r\n')
case "$stat_time_tokens" in
    [0-9]*' '[0-9]*' '[0-9]*) ;;
    *) fail "stat time-format tokens did not emit numeric timestamps" ;;
esac

touch_target="$WORK_DIR/touch_flags.txt"
printf 'touch\n' > "$touch_target"
touch_original_m=$("$ROOT_DIR/build/stat" -c '%Y' "$touch_target" | tr -d '\r\n')
assert_command_succeeds "$ROOT_DIR/build/touch" -a -d @1111111111 "$touch_target"
touch_after_a_only=$("$ROOT_DIR/build/stat" -c '%X %Y' "$touch_target" | tr -d '\r\n')
assert_text_equals "$touch_after_a_only" "1111111111 $touch_original_m" "touch -a should update access time without changing mtime"
assert_command_succeeds "$ROOT_DIR/build/touch" -m -d @1234567890 "$touch_target"
touch_after_m_only=$("$ROOT_DIR/build/stat" -c '%X %Y' "$touch_target" | tr -d '\r\n')
assert_text_equals "$touch_after_m_only" '1111111111 1234567890' "touch -m should preserve atime while updating mtime"
assert_command_succeeds "$ROOT_DIR/build/touch" -c "$WORK_DIR/not-created.txt"
[ ! -e "$WORK_DIR/not-created.txt" ] || fail "touch -c unexpectedly created a missing file"
touch_ref="$WORK_DIR/touch_reference.txt"
touch_copy="$WORK_DIR/touch_copy.txt"
assert_command_succeeds "$ROOT_DIR/build/touch" -d @1234567890 "$touch_ref"
assert_command_succeeds "$ROOT_DIR/build/touch" -r "$touch_ref" "$touch_copy"
touch_ref_times=$("$ROOT_DIR/build/stat" -c '%X %Y' "$touch_ref" | tr -d '\r\n')
touch_copy_times=$("$ROOT_DIR/build/stat" -c '%X %Y' "$touch_copy" | tr -d '\r\n')
assert_text_equals "$touch_copy_times" "$touch_ref_times" "touch -r did not copy the reference timestamps"
assert_command_succeeds "$ROOT_DIR/build/touch" -t 202401020304.05 "$WORK_DIR/touch_stamp.txt"
touch_stamp_time=$("$ROOT_DIR/build/stat" -c '%Y' "$WORK_DIR/touch_stamp.txt" | tr -d '\r\n')
assert_text_equals "$touch_stamp_time" '1704164645' "touch -t did not parse the timestamp correctly"

mkdir -p "$WORK_DIR/cp_src" "$WORK_DIR/cp_dest" "$WORK_DIR/mv_dest"
printf 'A' > "$WORK_DIR/cp_src/a.txt"
printf 'B' > "$WORK_DIR/cp_src/b.txt"
assert_command_succeeds "$ROOT_DIR/build/cp" "$WORK_DIR/cp_src/a.txt" "$WORK_DIR/cp_src/b.txt" "$WORK_DIR/cp_dest"
[ -f "$WORK_DIR/cp_dest/a.txt" ] || fail "cp multi-source mode did not copy the first file"
[ -f "$WORK_DIR/cp_dest/b.txt" ] || fail "cp multi-source mode did not copy the second file"
assert_command_succeeds "$ROOT_DIR/build/mv" "$WORK_DIR/cp_dest/a.txt" "$WORK_DIR/cp_dest/b.txt" "$WORK_DIR/mv_dest"
[ -f "$WORK_DIR/mv_dest/a.txt" ] || fail "mv multi-source mode did not move the first file"
[ -f "$WORK_DIR/mv_dest/b.txt" ] || fail "mv multi-source mode did not move the second file"
"$ROOT_DIR/build/ls" -1F "$WORK_DIR" > "$WORK_DIR/ls_flags.out"
assert_file_contains "$WORK_DIR/ls_flags.out" 'cp_src/' "ls -1F did not classify directories"
assert_command_succeeds "$ROOT_DIR/build/mkdir" -vm 700 "$WORK_DIR/private-dir"
"$ROOT_DIR/build/stat" "$WORK_DIR/private-dir" > "$WORK_DIR/private-dir.stat"
assert_file_contains "$WORK_DIR/private-dir.stat" 'Mode: drwx------' "mkdir -m failed to apply the requested mode"
assert_command_succeeds "$ROOT_DIR/build/mkdir" -vp "$WORK_DIR/rmdir_tree/a/b"
(
    cd "$WORK_DIR/rmdir_tree"
    "$ROOT_DIR/build/rmdir" -pv a/b
) > "$WORK_DIR/rmdir_pv.out"
assert_file_contains "$WORK_DIR/rmdir_pv.out" 'removed directory ' "rmdir -pv did not report removals"
[ ! -d "$WORK_DIR/rmdir_tree/a/b" ] || fail "rmdir -p left the nested directory behind"
[ ! -d "$WORK_DIR/rmdir_tree/a" ] || fail "rmdir -p left the parent directory behind"

printf 'keep\n' > "$WORK_DIR/rm_verbose.txt"
"$ROOT_DIR/build/rm" -v "$WORK_DIR/rm_verbose.txt" > "$WORK_DIR/rm_v.out"
assert_file_contains "$WORK_DIR/rm_v.out" 'removed ' "rm -v did not report the removal"
[ ! -e "$WORK_DIR/rm_verbose.txt" ] || fail "rm -v left the file behind"
mkdir -p "$WORK_DIR/rm_safe/sub"
printf 'safe\n' > "$WORK_DIR/rm_safe/sub/file.txt"
assert_command_succeeds "$ROOT_DIR/build/rm" -rf --preserve-root "$WORK_DIR/rm_safe"
[ ! -e "$WORK_DIR/rm_safe" ] || fail "rm --preserve-root should still remove normal directories"
mkdir -p "$WORK_DIR/rm_dot"
printf 'dot-keep\n' > "$WORK_DIR/rm_dot/file.txt"
if (
    cd "$WORK_DIR/rm_dot" &&
    "$ROOT_DIR/build/rm" -rf . >/dev/null 2>&1
); then
    fail "rm should refuse to remove ."
fi
[ -f "$WORK_DIR/rm_dot/file.txt" ] || fail "rm removed files even though . should be protected"

printf 'target-a\n' > "$WORK_DIR/link-a.txt"
printf 'target-b\n' > "$WORK_DIR/link-b.txt"
"$ROOT_DIR/build/ln" -sf link-a.txt "$WORK_DIR/force-link"
"$ROOT_DIR/build/ln" -sf link-b.txt "$WORK_DIR/force-link"
force_link_out=$("$ROOT_DIR/build/readlink" "$WORK_DIR/force-link" | tr -d '\r\n')
assert_text_equals "$force_link_out" 'link-b.txt' "ln -f did not replace the existing link"
mkdir -p "$WORK_DIR/link-dir"
"$ROOT_DIR/build/ln" -sv "$WORK_DIR/link-a.txt" "$WORK_DIR/link-b.txt" "$WORK_DIR/link-dir" > "$WORK_DIR/ln_v.out"
[ -L "$WORK_DIR/link-dir/link-a.txt" ] || fail "ln multi-source mode did not create the first symlink in the destination directory"
[ -L "$WORK_DIR/link-dir/link-b.txt" ] || fail "ln multi-source mode did not create the second symlink in the destination directory"
assert_file_contains "$WORK_DIR/ln_v.out" 'link-dir/link-a.txt' "ln -v did not describe the created link"
if "$ROOT_DIR/build/ln" -sT "$WORK_DIR/link-a.txt" "$WORK_DIR/link-dir" >/dev/null 2>&1; then
    fail "ln -T should refuse to treat a directory as a plain destination"
fi

"$ROOT_DIR/build/du" "$WORK_DIR" > "$WORK_DIR/du.out"
assert_file_contains "$WORK_DIR/du.out" '^[0-9][0-9]*[[:space:]].*extended_tools$' "du output missing directory total"
du_h_out=$("$ROOT_DIR/build/du" -sh "$WORK_DIR" | tr -d '\r')
printf '%s\n' "$du_h_out" > "$WORK_DIR/du_h.out"
assert_file_contains "$WORK_DIR/du_h.out" '^[0-9][0-9.]*[BKMGTP][[:space:]]' "du -h did not produce human-readable sizes"
"$ROOT_DIR/build/du" -a "$WORK_DIR/cp_src" > "$WORK_DIR/du_a.out"
assert_file_contains "$WORK_DIR/du_a.out" 'cp_src/a.txt$' "du -a did not include file entries"
"$ROOT_DIR/build/du" -d 0 "$WORK_DIR/cp_src" > "$WORK_DIR/du_d0.out"
if grep 'cp_src/a.txt$' "$WORK_DIR/du_d0.out" >/dev/null 2>&1; then
    fail "du -d 0 should suppress deeper child entries"
fi
"$ROOT_DIR/build/du" -ac "$WORK_DIR/cp_src" "$WORK_DIR/mv_dest" > "$WORK_DIR/du_ac.out"
assert_file_contains "$WORK_DIR/du_ac.out" '[[:space:]]total$' "du -c did not print a grand total"
"$ROOT_DIR/build/df" > "$WORK_DIR/df.out"
assert_file_contains "$WORK_DIR/df.out" '^Filesystem[[:space:]]' "df header missing"
"$ROOT_DIR/build/df" -h > "$WORK_DIR/df_h.out"
assert_file_contains "$WORK_DIR/df_h.out" '^/[[:space:]][0-9][0-9.]*[BKMGTP]' "df -h did not produce human-readable sizes"
"$ROOT_DIR/build/df" -iT > "$WORK_DIR/df_it.out"
assert_file_contains "$WORK_DIR/df_it.out" '^Filesystem[[:space:]][[:space:]]*Type[[:space:]][[:space:]]*Inodes[[:space:]][[:space:]]*IUsed' "df -iT header missing inode/type columns"
"$ROOT_DIR/build/stat" -f "$WORK_DIR" > "$WORK_DIR/stat_fs.out"
assert_file_contains "$WORK_DIR/stat_fs.out" '^Filesystem:' "stat -f did not print filesystem information"

chown_target="$WORK_DIR/chown.txt"
printf 'owner\n' > "$chown_target"
assert_command_succeeds "$ROOT_DIR/build/chown" "$(id -u):$(id -g)" "$chown_target"
assert_command_succeeds "$ROOT_DIR/build/chown" "$(id -un):$(id -gn)" "$chown_target"
mkdir -p "$WORK_DIR/chown_tree/nested"
printf 'owner\n' > "$WORK_DIR/chown_tree/nested/file.txt"
assert_command_succeeds "$ROOT_DIR/build/chown" -R "$(id -un):$(id -gn)" "$WORK_DIR/chown_tree"
"$ROOT_DIR/build/stat" "$WORK_DIR/chown_tree/nested/file.txt" > "$WORK_DIR/chown_recursive.stat"
assert_file_contains "$WORK_DIR/chown_recursive.stat" "^Owner: $(id -un)$" "chown -R did not preserve the requested owner name on nested files"
assert_file_contains "$WORK_DIR/chown_recursive.stat" "^Group: $(id -gn)$" "chown name lookup did not preserve the requested group on nested files"
chmod_target="$WORK_DIR/chmod_symbolic.txt"
printf 'chmod\n' > "$chmod_target"
assert_command_succeeds "$ROOT_DIR/build/chmod" u=rw,go= "$chmod_target"
"$ROOT_DIR/build/stat" "$chmod_target" > "$WORK_DIR/chmod_symbolic.stat"
assert_file_contains "$WORK_DIR/chmod_symbolic.stat" 'Mode: -rw-------' "chmod symbolic mode parsing failed"

id_uid_out=$("$ROOT_DIR/build/id" -u | tr -d '\r\n')
assert_text_equals "$id_uid_out" "$(id -u)" "id -u reported the wrong uid"
id_user_out=$("$ROOT_DIR/build/id" -un | tr -d '\r\n')
assert_text_equals "$id_user_out" "$(id -un)" "id -un reported the wrong username"
id_groups_out=$("$ROOT_DIR/build/id" -Gn | tr -d '\r\n')
[ -n "$id_groups_out" ] || fail "id -Gn produced no groups"

"$ROOT_DIR/build/sleep" 30 &
kill_pid=$!
"$ROOT_DIR/build/kill" --signal TERM "$kill_pid"
wait "$kill_pid" 2>/dev/null || true
if kill -0 "$kill_pid" 2>/dev/null; then
    fail "kill did not terminate the process"
fi
"$ROOT_DIR/build/kill" -l > "$WORK_DIR/kill_signals.out"
assert_file_contains "$WORK_DIR/kill_signals.out" 'TERM' "kill -l did not list signal names"
kill_term_num=$("$ROOT_DIR/build/kill" -l TERM | tr -d '\r\n')
assert_text_equals "$kill_term_num" '15' "kill -l TERM did not return the expected signal number"
kill_term_name=$("$ROOT_DIR/build/kill" -l 143 | tr -d '\r\n')
assert_text_equals "$kill_term_name" 'TERM' "kill -l 143 did not resolve the shell-status signal name"
"$ROOT_DIR/build/ps" -p $$ > "$WORK_DIR/ps.out"
assert_file_contains "$WORK_DIR/ps.out" '^PID' "ps header missing"
assert_file_contains "$WORK_DIR/ps.out" "^$$[[:space:]]" "ps -p did not include the current shell"
"$ROOT_DIR/build/pstree" $$ > "$WORK_DIR/pstree.out"
assert_file_contains "$WORK_DIR/pstree.out" "$$" "pstree did not render the current shell tree"
if "$ROOT_DIR/build/timeout" --signal=term --kill-after=200ms 0.2s "$ROOT_DIR/build/sleep" 3; then
    fail "timeout should have interrupted a long-running command"
else
    timed_status=$?
    [ "$timed_status" -eq 124 ] || fail "timeout returned the wrong exit code"
fi
"$ROOT_DIR/build/ping" -c 1 -i 0 -W 1 -s 0 -t 64 127.0.0.1 > "$WORK_DIR/ping_loopback.out" 2>&1 || true
if grep '^Usage:' "$WORK_DIR/ping_loopback.out" >/dev/null 2>&1; then
    fail "ping rejected its supported option set"
fi
[ -s "$WORK_DIR/ping_loopback.out" ] || fail "ping loopback smoke test produced no output"
if "$ROOT_DIR/build/ping" -c 0 127.0.0.1 > "$WORK_DIR/ping_bad_count.out" 2>&1; then
    fail "ping accepted a zero packet count"
fi
assert_file_contains "$WORK_DIR/ping_bad_count.out" '^Usage: ' "ping bad count should print usage"
if "$ROOT_DIR/build/ping" -W 0 127.0.0.1 > "$WORK_DIR/ping_bad_timeout.out" 2>&1; then
    fail "ping accepted a zero timeout"
fi
assert_file_contains "$WORK_DIR/ping_bad_timeout.out" '^Usage: ' "ping bad timeout should print usage"
if "$ROOT_DIR/build/ping" -s 1401 127.0.0.1 > "$WORK_DIR/ping_bad_size.out" 2>&1; then
    fail "ping accepted an oversized payload"
fi
assert_file_contains "$WORK_DIR/ping_bad_size.out" '^Usage: ' "ping bad payload should print usage"
if "$ROOT_DIR/build/ping" -t 256 127.0.0.1 > "$WORK_DIR/ping_bad_ttl.out" 2>&1; then
    fail "ping accepted an out-of-range TTL"
fi
assert_file_contains "$WORK_DIR/ping_bad_ttl.out" '^Usage: ' "ping bad TTL should print usage"

printf 'alpha one\nbeta two three\nalpha four\n' > "$WORK_DIR/awk.txt"
"$ROOT_DIR/build/awk" 'BEGIN { print "begin" } /alpha/ { print NR, NF, $2 } END { print "end", NR }' "$WORK_DIR/awk.txt" > "$WORK_DIR/awk.out"
cat > "$WORK_DIR/awk.expected" <<'EOF'
begin
1 2 one
3 2 four
end 3
EOF
assert_files_equal "$WORK_DIR/awk.expected" "$WORK_DIR/awk.out" "awk BEGIN/END/pattern/NR/NF behavior failed"
awk_nf_out=$("$ROOT_DIR/build/awk" 'NF == 3 { print NR, $1 }' "$WORK_DIR/awk.txt" | tr -d '\r\n')
assert_text_equals "$awk_nf_out" '2 beta' "awk NF condition failed"

printf '\tlead\nA\tB\n' | "$ROOT_DIR/build/expand" -i -t 4 > "$WORK_DIR/expand_i.out"
printf '    lead\nA\tB\n' > "$WORK_DIR/expand_i.expected"
assert_files_equal "$WORK_DIR/expand_i.expected" "$WORK_DIR/expand_i.out" "expand -i did not limit expansion to leading tabs"
expand_list_out=$(printf '1\t2\t3\n' | "$ROOT_DIR/build/expand" -t 4,6,10 | tr -d '\r\n')
assert_text_equals "$expand_list_out" '1   2 3' "expand tab-stop lists were not applied correctly"
printf '    lead\nA    B\n' | "$ROOT_DIR/build/unexpand" -a -i -t 4 > "$WORK_DIR/unexpand_i.out"
printf '\tlead\nA    B\n' > "$WORK_DIR/unexpand_i.expected"
assert_files_equal "$WORK_DIR/unexpand_i.expected" "$WORK_DIR/unexpand_i.out" "unexpand -i should override -a and preserve interior spaces"
unexpand_list_out=$(printf '    12  3\n' | "$ROOT_DIR/build/unexpand" -a -t 4,6,10 | tr -d '\r\n')
assert_text_equals "$unexpand_list_out" '	12  3' "unexpand tab-stop list handling failed"

printf 'A\n' > "$WORK_DIR/paste_a.txt"
printf '1\n' > "$WORK_DIR/paste_1.txt"
printf 'x\n' > "$WORK_DIR/paste_x.txt"
paste_escape_out=$("$ROOT_DIR/build/paste" -d '\t:' "$WORK_DIR/paste_a.txt" "$WORK_DIR/paste_1.txt" "$WORK_DIR/paste_x.txt" | tr -d '\r\n')
assert_text_equals "$paste_escape_out" 'A	1:x' "paste escaped delimiters were not decoded correctly"
printf 'a\nb\nc\n' > "$WORK_DIR/paste_serial.txt"
paste_serial_out=$("$ROOT_DIR/build/paste" -s -d ',\0' "$WORK_DIR/paste_serial.txt" | tr -d '\r\n')
assert_text_equals "$paste_serial_out" 'a,bc' "paste serial delimiter cycling with \\0 failed"
set --
i=1
while [ "$i" -le 18 ]; do
    file="$WORK_DIR/paste_many_$i.txt"
    printf '%s\n' "$i" > "$file"
    set -- "$@" "$file"
    i=$((i + 1))
done
"$ROOT_DIR/build/paste" -d ',' "$@" > "$WORK_DIR/paste_many.out"
assert_file_contains "$WORK_DIR/paste_many.out" '1,2,3,4,5' "paste did not accept a practical number of input files"
assert_file_contains "$WORK_DIR/paste_many.out" '16,17,18' "paste output was truncated when many files were provided"
printf 'L\0R\0' > "$WORK_DIR/paste_zero_left.txt"
printf '1\0002\000' > "$WORK_DIR/paste_zero_right.txt"
"$ROOT_DIR/build/paste" -z -d ':' "$WORK_DIR/paste_zero_left.txt" "$WORK_DIR/paste_zero_right.txt" | tr '\0' '\n' > "$WORK_DIR/paste_zero.out"
cat > "$WORK_DIR/paste_zero.expected" <<'EOF'
L:1
R:2
EOF
assert_files_equal "$WORK_DIR/paste_zero.expected" "$WORK_DIR/paste_zero.out" "paste -z did not join NUL-delimited records"

printf '\t12\n' | "$ROOT_DIR/build/fold" -w 4 > "$WORK_DIR/fold_columns.out"
printf '\t\n12\n' > "$WORK_DIR/fold_columns.expected"
assert_files_equal "$WORK_DIR/fold_columns.expected" "$WORK_DIR/fold_columns.out" "fold should count screen columns by default"
printf '\t12\n' | "$ROOT_DIR/build/fold" -b -w 4 > "$WORK_DIR/fold_bytes.out"
printf '\t12\n' > "$WORK_DIR/fold_bytes.expected"
assert_files_equal "$WORK_DIR/fold_bytes.expected" "$WORK_DIR/fold_bytes.out" "fold -b should count bytes instead of display columns"
printf 'abcdef\n' | "$ROOT_DIR/build/fold" -3 > "$WORK_DIR/fold_short.out"
printf 'abc\ndef\n' > "$WORK_DIR/fold_short.expected"
assert_files_equal "$WORK_DIR/fold_short.expected" "$WORK_DIR/fold_short.out" "fold legacy -WIDTH form failed"

cat > "$WORK_DIR/column.txt" <<'EOF'
name:role:team
Ada:Eng:Kernel
Bob:Ops:Infra
EOF
"$ROOT_DIR/build/column" -t -s ':' -o ' | ' "$WORK_DIR/column.txt" > "$WORK_DIR/column.out"
cat > "$WORK_DIR/column.expected" <<'EOF'
name | role | team
Ada  | Eng  | Kernel
Bob  | Ops  | Infra
EOF
assert_files_equal "$WORK_DIR/column.expected" "$WORK_DIR/column.out" "column output styling or table alignment failed"
"$ROOT_DIR/build/column" -t -s ':' -o ' | ' -c 12 "$WORK_DIR/column.txt" > "$WORK_DIR/column_narrow.out"
column_first_line=$("$ROOT_DIR/build/head" -n 1 "$WORK_DIR/column_narrow.out" | tr -d '\r\n')
assert_text_equals "$column_first_line" 'name | role' "column -c did not limit the rendered table width"

cat > "$WORK_DIR/join_left.txt" <<'EOF'
k1:left
k2:solo
EOF
cat > "$WORK_DIR/join_right.txt" <<'EOF'
k1:right
k3:extra
EOF
"$ROOT_DIR/build/join" -t ':' -o 0,1.2,2.2 -e NONE "$WORK_DIR/join_left.txt" "$WORK_DIR/join_right.txt" > "$WORK_DIR/join.out"
cat > "$WORK_DIR/join.expected" <<'EOF'
k1:left:right
EOF
assert_files_equal "$WORK_DIR/join.expected" "$WORK_DIR/join.out" "join -o output selection failed"
"$ROOT_DIR/build/join" -t ':' -v1 -o 0,1.2,2.2 -e NONE "$WORK_DIR/join_left.txt" "$WORK_DIR/join_right.txt" > "$WORK_DIR/join_v1.out"
cat > "$WORK_DIR/join_v1.expected" <<'EOF'
k2:solo:NONE
EOF
assert_files_equal "$WORK_DIR/join_v1.expected" "$WORK_DIR/join_v1.out" "join -v1/-e handling failed"

mkdir -p "$WORK_DIR/tar_src"
printf 'archive-data\n' > "$WORK_DIR/tar_src/file.txt"
(
    cd "$WORK_DIR"
    "$ROOT_DIR/build/tar" -czf test.tar.gz tar_src
    rm -rf tar_src
    mkdir -p tar_extract_gz
    cd tar_extract_gz
    "$ROOT_DIR/build/tar" -xzf ../test.tar.gz
)
assert_file_contains "$WORK_DIR/tar_extract_gz/tar_src/file.txt" 'archive-data' "tar gzip integration failed"

printf 'streamed-data\n' | "$ROOT_DIR/build/gzip" -c | "$ROOT_DIR/build/gunzip" -c > "$WORK_DIR/gzip_stream.txt"
assert_file_contains "$WORK_DIR/gzip_stream.txt" 'streamed-data' "gzip/gunzip streaming pipeline failed"
printf 'level-data\n' | "$ROOT_DIR/build/gzip" -9 -c | "$ROOT_DIR/build/gunzip" -c > "$WORK_DIR/gzip_level.txt"
assert_file_contains "$WORK_DIR/gzip_level.txt" 'level-data' "gzip -9 compatibility parsing regressed"
printf 'keep-data\n' > "$WORK_DIR/gzip_keep.txt"
assert_command_succeeds "$ROOT_DIR/build/gzip" -k "$WORK_DIR/gzip_keep.txt"
[ -f "$WORK_DIR/gzip_keep.txt" ] || fail "gzip -k should keep the original input file"
[ -f "$WORK_DIR/gzip_keep.txt.gz" ] || fail "gzip -k did not create the compressed output"
if "$ROOT_DIR/build/gzip" "$WORK_DIR/gzip_keep.txt" >/dev/null 2>&1; then
    fail "gzip should refuse to overwrite an existing .gz output without -f"
fi
assert_command_succeeds "$ROOT_DIR/build/gzip" -f "$WORK_DIR/gzip_keep.txt"
[ ! -e "$WORK_DIR/gzip_keep.txt" ] || fail "gzip should remove the input file after successful compression"
[ -f "$WORK_DIR/gzip_keep.txt.gz" ] || fail "gzip -f did not leave the compressed output behind"
assert_command_succeeds "$ROOT_DIR/build/gunzip" -k "$WORK_DIR/gzip_keep.txt.gz"
[ -f "$WORK_DIR/gzip_keep.txt.gz" ] || fail "gunzip -k should keep the compressed input file"
assert_file_contains "$WORK_DIR/gzip_keep.txt" '^keep-data$' "gunzip -k did not restore the original data"
printf 'fresh-data\n' > "$WORK_DIR/gunzip_force.txt"
assert_command_succeeds "$ROOT_DIR/build/gzip" "$WORK_DIR/gunzip_force.txt"
printf 'stale-data\n' > "$WORK_DIR/gunzip_force.txt"
if "$ROOT_DIR/build/gunzip" "$WORK_DIR/gunzip_force.txt.gz" >/dev/null 2>&1; then
    fail "gunzip should refuse to overwrite an existing destination without -f"
fi
assert_command_succeeds "$ROOT_DIR/build/gunzip" -f "$WORK_DIR/gunzip_force.txt.gz"
[ ! -e "$WORK_DIR/gunzip_force.txt.gz" ] || fail "gunzip should remove the compressed input after successful extraction"
assert_file_contains "$WORK_DIR/gunzip_force.txt" '^fresh-data$' "gunzip -f did not overwrite the destination with decompressed content"

printf 'checksum-data\n' > "$WORK_DIR/checksum.txt"
"$ROOT_DIR/build/md5sum" "$WORK_DIR/checksum.txt" > "$WORK_DIR/checksum.md5"
assert_command_succeeds "$ROOT_DIR/build/md5sum" -c "$WORK_DIR/checksum.md5"
"$ROOT_DIR/build/sha256sum" -z "$WORK_DIR/checksum.txt" > "$WORK_DIR/checksum.sha256z"
tr '\0' '\n' < "$WORK_DIR/checksum.sha256z" > "$WORK_DIR/checksum.sha256z.txt"
assert_file_contains "$WORK_DIR/checksum.sha256z.txt" 'checksum.txt' "sha256sum -z output missing filename"
assert_command_succeeds "$ROOT_DIR/build/sha256sum" -c -z "$WORK_DIR/checksum.sha256z"
"$ROOT_DIR/build/sha512sum" "$WORK_DIR/checksum.txt" > "$WORK_DIR/checksum.sha512"
assert_command_succeeds "$ROOT_DIR/build/sha512sum" --status -c "$WORK_DIR/checksum.sha512"

mkdir -p "$WORK_DIR/tar_base/src_dir" "$WORK_DIR/tar_dest"
printf 'tar-c-data\n' > "$WORK_DIR/tar_base/src_dir/nested.txt"
(
    cd "$WORK_DIR"
    "$ROOT_DIR/build/tar" -cvf cdir.tar -C tar_base src_dir > tar_verbose.out
    "$ROOT_DIR/build/tar" -xvf cdir.tar -C tar_dest > tar_extract_verbose.out
)
assert_file_contains "$WORK_DIR/tar_verbose.out" 'src_dir/nested.txt' "tar verbose create output missing file"
assert_file_contains "$WORK_DIR/tar_extract_verbose.out" 'src_dir/nested.txt' "tar verbose extract output missing file"
assert_file_contains "$WORK_DIR/tar_dest/src_dir/nested.txt" 'tar-c-data' "tar -C extraction failed"

printf 'alpha\nbeta\n' > "$WORK_DIR/patch_target.txt"
cat > "$WORK_DIR/patch.diff" <<'EOF'
--- a/patch_target.txt
+++ b/patch_target.txt
@@ -1,2 +1,2 @@
 alpha
-beta
+gamma
EOF
(
    cd "$WORK_DIR"
    cat patch.diff | "$ROOT_DIR/build/patch" -p1
    "$ROOT_DIR/build/patch" -R -p1 -i patch.diff
)
assert_file_contains "$WORK_DIR/patch_target.txt" '^alpha$' "patch did not preserve the first line"
assert_file_contains "$WORK_DIR/patch_target.txt" '^beta$' "patch reverse apply failed"
(
    cd "$WORK_DIR"
    "$ROOT_DIR/build/patch" -b -p1 -i patch.diff
)
assert_file_contains "$WORK_DIR/patch_target.txt" '^gamma$' "patch -b did not apply the change"
assert_file_contains "$WORK_DIR/patch_target.txt.orig" '^beta$' "patch -b did not create a backup of the original file"
printf 'alpha\nbeta\n' > "$WORK_DIR/patch_target.txt"
(
    cd "$WORK_DIR"
    "$ROOT_DIR/build/patch" -p1 -o patch_preview.txt -i patch.diff
)
assert_file_contains "$WORK_DIR/patch_preview.txt" '^gamma$' "patch -o did not write the patched output file"
assert_file_contains "$WORK_DIR/patch_target.txt" '^beta$' "patch -o should leave the original target unchanged"

mkdir -p "$WORK_DIR/make_include"
cat > "$WORK_DIR/make_include/makefile" <<'EOF'
MSG ?= fallback
include config.mk
MSG += value
all:
	printf '%s\n' "$(MSG)" > built.txt
EOF
cat > "$WORK_DIR/make_include/config.mk" <<'EOF'
MSG := included
EOF
(
    cd "$WORK_DIR/make_include"
    "$ROOT_DIR/build/make"
)
assert_file_contains "$WORK_DIR/make_include/built.txt" '^included value$' "make include/default makefile handling failed"
mkdir -p "$WORK_DIR/make_pattern"
cat > "$WORK_DIR/make_pattern/Makefile" <<'EOF'
all: result.txt

%.txt: %.in
	printf 'stem=%s src=%s\n' "$*" "$<" > "$@"
EOF
printf 'pattern-input\n' > "$WORK_DIR/make_pattern/result.in"
(
    cd "$WORK_DIR/make_pattern"
    "$ROOT_DIR/build/make"
)
assert_file_contains "$WORK_DIR/make_pattern/result.txt" '^stem=result src=result.in$' "make pattern rule or automatic variables failed"

mktemp_path=$(TMPDIR="$WORK_DIR" "$ROOT_DIR/build/mktemp" -t wavecheck | tr -d '\r\n')
case "$mktemp_path" in
    "$WORK_DIR"/wavecheck.*) ;;
    *) fail "mktemp -t did not honor TMPDIR/prefix" ;;
esac
[ -f "$mktemp_path" ] || fail "mktemp -t did not create the requested file"

"$ROOT_DIR/build/netcat" -l 24681 > "$WORK_DIR/netcat_server.out" &
netcat_pid=$!
sleep 1
printf 'hello nc\n' | "$ROOT_DIR/build/netcat" localhost 24681 > "$WORK_DIR/netcat_client.out"
wait "$netcat_pid"
assert_file_contains "$WORK_DIR/netcat_server.out" 'hello nc' "netcat listener did not receive payload"
"$ROOT_DIR/build/netcat" -u -l -w 3 24682 > "$WORK_DIR/netcat_udp_server.out" &
netcat_udp_pid=$!
sleep 1
printf 'hello udp\n' | "$ROOT_DIR/build/netcat" -u -w 1 localhost 24682 > "$WORK_DIR/netcat_udp_client.out"
wait "$netcat_udp_pid"
assert_file_contains "$WORK_DIR/netcat_udp_server.out" 'hello udp' "netcat UDP mode did not receive the payload"
"$ROOT_DIR/build/netcat" -l -w 3 24683 > "$WORK_DIR/netcat_scan_server.out" &
netcat_scan_pid=$!
sleep 1
assert_command_succeeds "$ROOT_DIR/build/netcat" -z -w 1 localhost 24683
wait "$netcat_scan_pid"
if "$ROOT_DIR/build/netcat" -z -w 1 localhost 24684; then
    fail "netcat -z unexpectedly succeeded against a closed port"
fi
"$ROOT_DIR/build/free" -h > "$WORK_DIR/free.out"
assert_file_contains "$WORK_DIR/free.out" '^Mem:' "free -h output missing memory line"
"$ROOT_DIR/build/free" -wt > "$WORK_DIR/free_wt.out"
assert_file_contains "$WORK_DIR/free_wt.out" '^Swap:' "free -wt output missing swap line"
assert_file_contains "$WORK_DIR/free_wt.out" '^Total:' "free -wt output missing total line"
"$ROOT_DIR/build/uptime" -p > "$WORK_DIR/uptime.out"
assert_file_contains "$WORK_DIR/uptime.out" '^up ' "uptime -p output missing pretty prefix"
assert_file_contains "$WORK_DIR/uptime.out" 'day\|hour\|minute\|second' "uptime -p did not use human-friendly units"
"$ROOT_DIR/build/uptime" -s > "$WORK_DIR/uptime_since.out"
assert_file_contains "$WORK_DIR/uptime_since.out" '^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9] ' "uptime -s output missing timestamp"
"$ROOT_DIR/build/uptime" > "$WORK_DIR/uptime_default.out"
assert_file_contains "$WORK_DIR/uptime_default.out" 'load average:' "uptime default output missing load averages"
"$ROOT_DIR/build/who" -q > "$WORK_DIR/who_q.out"
assert_file_contains "$WORK_DIR/who_q.out" '^# users=' "who -q output missing user count"
"$ROOT_DIR/build/who" -b > "$WORK_DIR/who_b.out"
assert_file_contains "$WORK_DIR/who_b.out" '^system boot  ' "who -b output missing boot line"
users_count_out=$("$ROOT_DIR/build/users" -cu | tr -d '\r\n')
case "$users_count_out" in
    ''|*[!0-9]*) fail "users -cu did not return a numeric count" ;;
esac
groups_primary_out=$("$ROOT_DIR/build/groups" -dn | tr -d '\r\n')
assert_text_equals "$groups_primary_out" "$(id -g)" "groups -dn reported the wrong primary gid"
printenv_name_out=$(REPORTING_WAVE_CHECK=ready "$ROOT_DIR/build/printenv" -n REPORTING_WAVE_CHECK | tr -d '\r\n')
assert_text_equals "$printenv_name_out" 'REPORTING_WAVE_CHECK' "printenv -n did not emit the requested name"
assert_command_succeeds env REPORTING_WAVE_CHECK=ready "$ROOT_DIR/build/printenv" -q REPORTING_WAVE_CHECK
if env REPORTING_WAVE_CHECK=ready "$ROOT_DIR/build/printenv" -q REPORTING_WAVE_MISSING; then
    fail "printenv -q should fail for a missing variable"
fi
