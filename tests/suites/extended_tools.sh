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
uname_out=$("$ROOT_DIR/build/uname" -snrm | tr -d '\r\n')
case "$uname_out" in
    *" "*) ;;
    *) fail "uname flag output was unexpectedly thin" ;;
esac

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
if "$ROOT_DIR/build/diff" -u "$WORK_DIR/left.txt" "$WORK_DIR/right.txt" > "$WORK_DIR/diff_u.out"; then
    fail "diff -u should have reported a difference"
fi
assert_file_contains "$WORK_DIR/diff_u.out" '^--- ' "diff -u header missing"
assert_file_contains "$WORK_DIR/diff_u.out" '^@@ ' "diff -u hunk header missing"

"$ROOT_DIR/build/file" "$WORK_DIR/left.txt" > "$WORK_DIR/file.out"
assert_file_contains "$WORK_DIR/file.out" 'ASCII text' "file type detection failed"

printf '\001\002HelloString\000xxMoreText\n' > "$WORK_DIR/strings.bin"
"$ROOT_DIR/build/strings" "$WORK_DIR/strings.bin" > "$WORK_DIR/strings.out"
assert_file_contains "$WORK_DIR/strings.out" 'HelloString' "strings output missing printable sequence"

printf_out=$("$ROOT_DIR/build/printf" 'value=%s %d %x\n' sample 42 255 | tr -d '\r')
assert_text_equals "$printf_out" 'value=sample 42 ff' "printf formatting failed"
echo_out=$("$ROOT_DIR/build/echo" -n sample | tr -d '\r')
assert_text_equals "$echo_out" 'sample' "echo -n failed"

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

printf 'name:role:team\nAda:Eng:Kernel\nBob:Ops:Infra\n' > "$WORK_DIR/cut_fields.txt"
"$ROOT_DIR/build/cut" -d ':' -f 1,3 "$WORK_DIR/cut_fields.txt" > "$WORK_DIR/cut_fields.out"
cat > "$WORK_DIR/cut_fields.expected" <<'EOF'
name:team
Ada:Kernel
Bob:Infra
EOF
assert_files_equal "$WORK_DIR/cut_fields.expected" "$WORK_DIR/cut_fields.out" "cut field mode failed"

tr_delete=$(printf 'a1b22c333\n' | "$ROOT_DIR/build/tr" -d '0-9' | tr -d '\r\n')
assert_text_equals "$tr_delete" 'abc' "tr delete range failed"
tr_squeeze=$(printf 'aa   bb    cc\n' | "$ROOT_DIR/build/tr" -s ' ' | tr -d '\r\n')
assert_text_equals "$tr_squeeze" 'aa bb cc' "tr squeeze mode failed"

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

ln -sf dd.in "$WORK_DIR/link-to-dd"
readlink_out=$("$ROOT_DIR/build/readlink" "$WORK_DIR/link-to-dd" | tr -d '\r\n')
assert_text_equals "$readlink_out" 'dd.in' "readlink target mismatch"
"$ROOT_DIR/build/stat" "$WORK_DIR/dd.in" > "$WORK_DIR/stat.out"
assert_file_contains "$WORK_DIR/stat.out" '^Size:' "stat output missing size"
assert_file_contains "$WORK_DIR/stat.out" 'Type: file' "stat output missing file type"

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
assert_command_succeeds "$ROOT_DIR/build/mkdir" -m 700 "$WORK_DIR/private-dir"
"$ROOT_DIR/build/stat" "$WORK_DIR/private-dir" > "$WORK_DIR/private-dir.stat"
assert_file_contains "$WORK_DIR/private-dir.stat" 'Mode: drwx------' "mkdir -m failed to apply the requested mode"

printf 'keep\n' > "$WORK_DIR/rm_verbose.txt"
"$ROOT_DIR/build/rm" -v "$WORK_DIR/rm_verbose.txt" > "$WORK_DIR/rm_v.out"
assert_file_contains "$WORK_DIR/rm_v.out" 'removed ' "rm -v did not report the removal"
[ ! -e "$WORK_DIR/rm_verbose.txt" ] || fail "rm -v left the file behind"

printf 'target-a\n' > "$WORK_DIR/link-a.txt"
printf 'target-b\n' > "$WORK_DIR/link-b.txt"
"$ROOT_DIR/build/ln" -sf link-a.txt "$WORK_DIR/force-link"
"$ROOT_DIR/build/ln" -sf link-b.txt "$WORK_DIR/force-link"
force_link_out=$("$ROOT_DIR/build/readlink" "$WORK_DIR/force-link" | tr -d '\r\n')
assert_text_equals "$force_link_out" 'link-b.txt' "ln -f did not replace the existing link"

"$ROOT_DIR/build/du" "$WORK_DIR" > "$WORK_DIR/du.out"
assert_file_contains "$WORK_DIR/du.out" '^[0-9][0-9]*[[:space:]].*extended_tools$' "du output missing directory total"
du_h_out=$("$ROOT_DIR/build/du" -sh "$WORK_DIR" | tr -d '\r')
printf '%s\n' "$du_h_out" > "$WORK_DIR/du_h.out"
assert_file_contains "$WORK_DIR/du_h.out" '^[0-9][0-9.]*[BKMGTP][[:space:]]' "du -h did not produce human-readable sizes"
"$ROOT_DIR/build/df" > "$WORK_DIR/df.out"
assert_file_contains "$WORK_DIR/df.out" '^Filesystem[[:space:]]' "df header missing"
"$ROOT_DIR/build/df" -h > "$WORK_DIR/df_h.out"
assert_file_contains "$WORK_DIR/df_h.out" '^/[[:space:]][0-9][0-9.]*[BKMGTP]' "df -h did not produce human-readable sizes"

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
"$ROOT_DIR/build/kill" -l > "$WORK_DIR/kill_signals.out"
assert_file_contains "$WORK_DIR/kill_signals.out" 'TERM' "kill -l did not list signal names"
"$ROOT_DIR/build/ps" -p $$ > "$WORK_DIR/ps.out"
assert_file_contains "$WORK_DIR/ps.out" '^PID' "ps header missing"
assert_file_contains "$WORK_DIR/ps.out" "^$$[[:space:]]" "ps -p did not include the current shell"
"$ROOT_DIR/build/pstree" $$ > "$WORK_DIR/pstree.out"
assert_file_contains "$WORK_DIR/pstree.out" "$$" "pstree did not render the current shell tree"
if "$ROOT_DIR/build/timeout" 1s "$ROOT_DIR/build/sleep" 3; then
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

"$ROOT_DIR/build/netcat" -l 24681 > "$WORK_DIR/netcat_server.out" &
netcat_pid=$!
sleep 1
printf 'hello nc\n' | "$ROOT_DIR/build/netcat" localhost 24681 > "$WORK_DIR/netcat_client.out"
wait "$netcat_pid"
assert_file_contains "$WORK_DIR/netcat_server.out" 'hello nc' "netcat listener did not receive payload"
"$ROOT_DIR/build/free" -h > "$WORK_DIR/free.out"
assert_file_contains "$WORK_DIR/free.out" '^Mem:' "free -h output missing memory line"
"$ROOT_DIR/build/uptime" -p > "$WORK_DIR/uptime.out"
assert_file_contains "$WORK_DIR/uptime.out" '^up ' "uptime -p output missing pretty prefix"
"$ROOT_DIR/build/who" -q > "$WORK_DIR/who_q.out"
assert_file_contains "$WORK_DIR/who_q.out" '^# users=' "who -q output missing user count"
