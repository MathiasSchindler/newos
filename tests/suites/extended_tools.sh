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

env_out=$(FOO=bar "$ROOT_DIR/build/env" | tr -d '\r')
case "$env_out" in
    *FOO=bar*) ;;
    *) fail "env output missing variable" ;;
esac
env_cmd_out=$("$ROOT_DIR/build/env" BAR=baz "$ROOT_DIR/build/sh" -c 'echo $BAR' | tr -d '\r\n')
assert_text_equals "$env_cmd_out" 'baz' "env command override failed"
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

printf 'hello tee\n' | "$ROOT_DIR/build/tee" "$WORK_DIR/tee.txt" > "$WORK_DIR/tee.out"
assert_files_equal "$WORK_DIR/tee.txt" "$WORK_DIR/tee.out" "tee output mismatch"

printf 'one two\n' | "$ROOT_DIR/build/xargs" "$ROOT_DIR/build/echo" prefix > "$WORK_DIR/xargs.out"
actual_xargs=$(tr -d '\r' < "$WORK_DIR/xargs.out" | head -n 1)
assert_text_equals "$actual_xargs" 'prefix one two' "xargs argument composition failed"
printf 'copy-a\n' > "$WORK_DIR/cp_a.txt"
printf 'copy-b\n' > "$WORK_DIR/cp_b.txt"
mkdir -p "$WORK_DIR/cp_dest"
assert_command_succeeds "$ROOT_DIR/build/cp" "$WORK_DIR/cp_a.txt" "$WORK_DIR/cp_b.txt" "$WORK_DIR/cp_dest"
assert_file_contains "$WORK_DIR/cp_dest/cp_a.txt" 'copy-a' "cp multi-source copy failed for first file"
assert_file_contains "$WORK_DIR/cp_dest/cp_b.txt" 'copy-b' "cp multi-source copy failed for second file"
printf 'move-a\n' > "$WORK_DIR/mv_a.txt"
printf 'move-b\n' > "$WORK_DIR/mv_b.txt"
mkdir -p "$WORK_DIR/mv_dest"
assert_command_succeeds "$ROOT_DIR/build/mv" "$WORK_DIR/mv_a.txt" "$WORK_DIR/mv_b.txt" "$WORK_DIR/mv_dest"
assert_file_contains "$WORK_DIR/mv_dest/mv_a.txt" 'move-a' "mv multi-source move failed for first file"
assert_file_contains "$WORK_DIR/mv_dest/mv_b.txt" 'move-b' "mv multi-source move failed for second file"
[ ! -e "$WORK_DIR/mv_a.txt" ] || fail "mv multi-source move left the original file behind"
[ ! -e "$WORK_DIR/mv_b.txt" ] || fail "mv multi-source move left the second original file behind"

printf 'abcdef' > "$WORK_DIR/dd.in"
"$ROOT_DIR/build/dd" if="$WORK_DIR/dd.in" of="$WORK_DIR/dd.out" bs=2 count=2
printf 'abcd' > "$WORK_DIR/dd.expected"
assert_files_equal "$WORK_DIR/dd.out" "$WORK_DIR/dd.expected" "dd block copy failed"

"$ROOT_DIR/build/od" "$WORK_DIR/dd.in" > "$WORK_DIR/od.out"
assert_file_contains "$WORK_DIR/od.out" '141 142 143 144' "od dump missing expected bytes"

"$ROOT_DIR/build/hexdump" "$WORK_DIR/dd.in" > "$WORK_DIR/hexdump.out"
assert_file_contains "$WORK_DIR/hexdump.out" '61 62 63 64' "hexdump missing expected hex bytes"

actual_basename=$("$ROOT_DIR/build/basename" /tmp/example.txt .txt | tr -d '\r\n')
assert_text_equals "$actual_basename" 'example' "basename failed"
actual_dirname=$("$ROOT_DIR/build/dirname" /tmp/example.txt | tr -d '\r\n')
assert_text_equals "$actual_dirname" '/tmp' "dirname failed"
actual_realpath=$("$ROOT_DIR/build/realpath" "$WORK_DIR/sub/../dd.in" | tr -d '\r\n')
assert_text_equals "$actual_realpath" "$WORK_DIR/dd.in" "realpath normalization failed"

assert_command_succeeds "$ROOT_DIR/build/cmp" "$WORK_DIR/dd.in" "$WORK_DIR/dd.in"
printf 'left\nline2\n' > "$WORK_DIR/left.txt"
printf 'left\nchanged\n' > "$WORK_DIR/right.txt"
if "$ROOT_DIR/build/diff" "$WORK_DIR/left.txt" "$WORK_DIR/right.txt" > "$WORK_DIR/diff.out"; then
    fail "diff should have reported a difference"
fi
assert_file_contains "$WORK_DIR/diff.out" '^line 2:$' "diff output missing changed line"

"$ROOT_DIR/build/file" "$WORK_DIR/left.txt" > "$WORK_DIR/file.out"
assert_file_contains "$WORK_DIR/file.out" 'ASCII text' "file type detection failed"

printf '\001\002HelloString\000xxMoreText\n' > "$WORK_DIR/strings.bin"
"$ROOT_DIR/build/strings" "$WORK_DIR/strings.bin" > "$WORK_DIR/strings.out"
assert_file_contains "$WORK_DIR/strings.out" 'HelloString' "strings output missing printable sequence"

printf_out=$("$ROOT_DIR/build/printf" 'value=%s %d %x\n' sample 42 255 | tr -d '\r')
assert_text_equals "$printf_out" 'value=sample 42 ff' "printf formatting failed"

which_out=$("$ROOT_DIR/build/which" ls | tr -d '\r\n')
assert_text_equals "$which_out" "$ROOT_DIR/build/ls" "which did not resolve build tool"
mkdir -p "$WORK_DIR/path1" "$WORK_DIR/path2"
printf '#!/bin/sh\necho one\n' > "$WORK_DIR/path1/dupcmd"
printf '#!/bin/sh\necho two\n' > "$WORK_DIR/path2/dupcmd"
chmod +x "$WORK_DIR/path1/dupcmd" "$WORK_DIR/path2/dupcmd"
which_all_out=$(PATH="$WORK_DIR/path1:$WORK_DIR/path2:$PATH" "$ROOT_DIR/build/which" -a dupcmd | tr -d '\r')
assert_file_contains "$WORK_DIR/left.txt" '^left$' "diff fixture missing"
printf '%s\n' "$which_all_out" > "$WORK_DIR/which_all.out"
assert_file_contains "$WORK_DIR/which_all.out" 'path1/dupcmd' "which -a missing first match"
assert_file_contains "$WORK_DIR/which_all.out" 'path2/dupcmd' "which -a missing second match"

ln -sf dd.in "$WORK_DIR/link-to-dd"
readlink_out=$("$ROOT_DIR/build/readlink" "$WORK_DIR/link-to-dd" | tr -d '\r\n')
assert_text_equals "$readlink_out" 'dd.in' "readlink target mismatch"
"$ROOT_DIR/build/stat" "$WORK_DIR/dd.in" > "$WORK_DIR/stat.out"
assert_file_contains "$WORK_DIR/stat.out" '^Size:' "stat output missing size"
assert_file_contains "$WORK_DIR/stat.out" 'Type: file' "stat output missing file type"

"$ROOT_DIR/build/du" "$WORK_DIR" > "$WORK_DIR/du.out"
assert_file_contains "$WORK_DIR/du.out" '^[0-9][0-9]*[[:space:]].*extended_tools$' "du output missing directory total"
"$ROOT_DIR/build/df" > "$WORK_DIR/df.out"
assert_file_contains "$WORK_DIR/df.out" '^Filesystem[[:space:]]' "df header missing"

chown_target="$WORK_DIR/chown.txt"
printf 'owner\n' > "$chown_target"
assert_command_succeeds "$ROOT_DIR/build/chown" "$(id -u):$(id -g)" "$chown_target"

"$ROOT_DIR/build/sleep" 30 &
kill_pid=$!
"$ROOT_DIR/build/kill" "$kill_pid"
wait "$kill_pid" 2>/dev/null || true
if kill -0 "$kill_pid" 2>/dev/null; then
    fail "kill did not terminate the process"
fi

printf 'Hello\nNope\nHello\n' > "$WORK_DIR/grep_count.txt"
grep_count=$("$ROOT_DIR/build/grep" -c Hello "$WORK_DIR/grep_count.txt" | tr -d '\r\n')
assert_text_equals "$grep_count" '2' "grep count mode failed"
if ! "$ROOT_DIR/build/grep" -q Hello "$WORK_DIR/grep_count.txt"; then
    fail "grep quiet mode failed"
fi
"$ROOT_DIR/build/grep" -l Hello "$WORK_DIR/grep_count.txt" > "$WORK_DIR/grep_l.out"
assert_file_contains "$WORK_DIR/grep_l.out" 'grep_count.txt' "grep list-files mode failed"

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

"$ROOT_DIR/build/netcat" -l 24681 > "$WORK_DIR/netcat_server.out" &
netcat_pid=$!
sleep 1
printf 'hello nc\n' | "$ROOT_DIR/build/netcat" 127.0.0.1 24681 > "$WORK_DIR/netcat_client.out"
wait "$netcat_pid"
assert_file_contains "$WORK_DIR/netcat_server.out" 'hello nc' "netcat listener did not receive payload"
