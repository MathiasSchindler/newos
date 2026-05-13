#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"

BIN_DIR=${NEWOS_INCEPTION_BUILD_DIR:-"$ROOT_DIR/build/inception-freestanding-$(uname -m)"}
WORK_DIR="$ROOT_DIR/tests/tmp/inception"

require_tool() {
    tool=$1
    [ -x "$BIN_DIR/$tool" ] || fail "inception build directory is missing expected tool: $BIN_DIR/$tool"
}

for tool in false true yes clear dirname echo basename tee whoami mkdir cat printenv od sleep strings sync hostname shutdown bunzip2 xz mknod bzip2 comm tac pwd tsort expand truncate unxz paste unexpand cut rmdir rev chmod groups readlink cmp umount uname uniq tr mktemp kill id logger realpath stty head ln free strip printf rm nl sha256sum shuf time xmlrecode which dd objdump chgrp gunzip sha512sum split df readelf who ar chown users mv test env pstree touch uptime wc tail seq md5sum cp stat date fold fmt timeout column '[' expr sort du gzip file diff less more watch nslookup xargs ps xmlmin patch xmlstrip xmlcount xmlfmt xml2yaml xmldiff dig xmltokens xmlcanon xml2json init ls xmlsafe xmlstats xml2lines getty netcat top xmlnscheck xmldtdinfo ping xmlget find; do
    require_tool "$tool"
done

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/fs/source" "$WORK_DIR/fs/copy"

note "inception basic execution"
"$BIN_DIR/true"
if "$BIN_DIR/false"; then
    fail "inception false unexpectedly succeeded"
fi
"$BIN_DIR/echo" "hello inception" > "$WORK_DIR/echo.out"
assert_file_contains "$WORK_DIR/echo.out" '^hello inception$' "inception echo did not run"
"$BIN_DIR/printf" '%s\n' gamma alpha beta > "$WORK_DIR/lines.in"

note "inception text tools"
"$BIN_DIR/sort" "$WORK_DIR/lines.in" > "$WORK_DIR/sort.out"
cat > "$WORK_DIR/sort.expected" <<'EOF'
alpha
beta
gamma
EOF
assert_files_equal "$WORK_DIR/sort.expected" "$WORK_DIR/sort.out" "inception sort output mismatch"
"$BIN_DIR/cmp" "$WORK_DIR/sort.expected" "$WORK_DIR/sort.out"
"$BIN_DIR/cat" "$WORK_DIR/lines.in" | "$BIN_DIR/tr" a-z A-Z > "$WORK_DIR/tr.out"
cat > "$WORK_DIR/tr.expected" <<'EOF'
GAMMA
ALPHA
BETA
EOF
assert_files_equal "$WORK_DIR/tr.expected" "$WORK_DIR/tr.out" "inception cat/tr pipeline mismatch"
"$BIN_DIR/wc" -l "$WORK_DIR/lines.in" > "$WORK_DIR/wc.out"
assert_file_contains "$WORK_DIR/wc.out" '3 .*lines\.in' "inception wc -l output mismatch"
"$BIN_DIR/head" -n 2 "$WORK_DIR/lines.in" > "$WORK_DIR/head.out"
"$BIN_DIR/tail" -n 2 "$WORK_DIR/lines.in" > "$WORK_DIR/tail.out"
"$BIN_DIR/seq" 3 5 > "$WORK_DIR/seq.out"
assert_file_contains "$WORK_DIR/seq.out" '^3$' "inception seq did not emit start"
assert_file_contains "$WORK_DIR/seq.out" '^5$' "inception seq did not emit end"
"$BIN_DIR/paste" "$WORK_DIR/head.out" "$WORK_DIR/tail.out" > "$WORK_DIR/paste.out"
"$BIN_DIR/comm" -12 "$WORK_DIR/sort.expected" "$WORK_DIR/sort.out" > "$WORK_DIR/comm.out"
assert_files_equal "$WORK_DIR/sort.expected" "$WORK_DIR/comm.out" "inception comm common output mismatch"
"$BIN_DIR/uniq" "$WORK_DIR/sort.out" > "$WORK_DIR/uniq.out"
assert_files_equal "$WORK_DIR/sort.expected" "$WORK_DIR/uniq.out" "inception uniq output mismatch"

note "inception filesystem tools"
"$BIN_DIR/cp" "$WORK_DIR/lines.in" "$WORK_DIR/fs/source/file.txt"
"$BIN_DIR/ln" "$WORK_DIR/fs/source/file.txt" "$WORK_DIR/fs/source/hardlink.txt"
"$BIN_DIR/ln" -s file.txt "$WORK_DIR/fs/source/symlink.txt"
"$BIN_DIR/cp" "$WORK_DIR/fs/source/file.txt" "$WORK_DIR/fs/copy/copied.txt"
"$BIN_DIR/mv" "$WORK_DIR/fs/copy/copied.txt" "$WORK_DIR/fs/copy/moved.txt"
"$BIN_DIR/test" -f "$WORK_DIR/fs/copy/moved.txt"
"$BIN_DIR/readlink" "$WORK_DIR/fs/source/symlink.txt" > "$WORK_DIR/readlink.out"
assert_file_contains "$WORK_DIR/readlink.out" '^file\.txt$' "inception readlink output mismatch"
"$BIN_DIR/stat" -c '%F %h' "$WORK_DIR/fs/source/file.txt" > "$WORK_DIR/stat.out"
assert_file_contains "$WORK_DIR/stat.out" '^file 2$' "inception stat hardlink count mismatch"
"$BIN_DIR/find" "$WORK_DIR/fs" -name moved.txt > "$WORK_DIR/find.out"
assert_file_contains "$WORK_DIR/find.out" 'moved\.txt$' "inception find did not locate moved file"
"$BIN_DIR/realpath" "$WORK_DIR/fs/source/../copy/moved.txt" > "$WORK_DIR/realpath.out"
assert_file_contains "$WORK_DIR/realpath.out" '/copy/moved\.txt$' "inception realpath output mismatch"
"$BIN_DIR/dirname" "$WORK_DIR/fs/copy/moved.txt" > "$WORK_DIR/dirname.out"
assert_file_contains "$WORK_DIR/dirname.out" '/copy$' "inception dirname output mismatch"
"$BIN_DIR/basename" "$WORK_DIR/fs/copy/moved.txt" > "$WORK_DIR/basename.out"
assert_file_contains "$WORK_DIR/basename.out" '^moved\.txt$' "inception basename output mismatch"

note "inception compression and checksums"
"$BIN_DIR/gzip" -c "$WORK_DIR/lines.in" | "$BIN_DIR/gunzip" -c > "$WORK_DIR/gzip.out"
assert_files_equal "$WORK_DIR/lines.in" "$WORK_DIR/gzip.out" "inception gzip/gunzip round trip mismatch"
"$BIN_DIR/cp" "$WORK_DIR/lines.in" "$WORK_DIR/bzip2.in"
"$BIN_DIR/bzip2" "$WORK_DIR/bzip2.in"
"$BIN_DIR/rm" "$WORK_DIR/bzip2.in"
"$BIN_DIR/bunzip2" "$WORK_DIR/bzip2.in.bz2"
assert_files_equal "$WORK_DIR/lines.in" "$WORK_DIR/bzip2.in" "inception bzip2/bunzip2 round trip mismatch"
"$BIN_DIR/cp" "$WORK_DIR/lines.in" "$WORK_DIR/xz.in"
"$BIN_DIR/xz" "$WORK_DIR/xz.in"
"$BIN_DIR/rm" "$WORK_DIR/xz.in"
"$BIN_DIR/unxz" "$WORK_DIR/xz.in.xz"
assert_files_equal "$WORK_DIR/lines.in" "$WORK_DIR/xz.in" "inception xz/unxz round trip mismatch"
"$BIN_DIR/md5sum" "$WORK_DIR/lines.in" > "$WORK_DIR/md5.out"
"$BIN_DIR/sha256sum" "$WORK_DIR/lines.in" > "$WORK_DIR/sha256.out"
"$BIN_DIR/sha512sum" "$WORK_DIR/lines.in" > "$WORK_DIR/sha512.out"
assert_file_contains "$WORK_DIR/md5.out" '  .*lines\.in$' "inception md5sum output missing filename"
assert_file_contains "$WORK_DIR/sha256.out" '  .*lines\.in$' "inception sha256sum output missing filename"
assert_file_contains "$WORK_DIR/sha512.out" '  .*lines\.in$' "inception sha512sum output missing filename"

note "inception system/status tools"
PATH="$BIN_DIR" "$BIN_DIR/which" sort > "$WORK_DIR/which.out"
assert_file_contains "$WORK_DIR/which.out" '/sort$' "inception which did not resolve sort"
"$BIN_DIR/date" -d @0 +%Y-%m-%d > "$WORK_DIR/date.out"
assert_file_contains "$WORK_DIR/date.out" '^1970-01-01$' "inception date epoch formatting mismatch"
"$BIN_DIR/free" > "$WORK_DIR/free.out"
"$BIN_DIR/uptime" > "$WORK_DIR/uptime.out"
"$BIN_DIR/ps" > "$WORK_DIR/ps.out"
"$BIN_DIR/pstree" --help > "$WORK_DIR/pstree.out" 2>&1
assert_file_contains "$WORK_DIR/pstree.out" '^Usage:' "inception pstree help missing"
"$BIN_DIR/init" --help > "$WORK_DIR/init.out" 2>&1
assert_file_contains "$WORK_DIR/init.out" '^Usage:' "inception init help missing"
"$BIN_DIR/getty" --help > "$WORK_DIR/getty.out" 2>&1
assert_file_contains "$WORK_DIR/getty.out" '^Usage:' "inception getty help missing"

note "inception xml tools"
cat > "$WORK_DIR/sample.xml" <<'EOF'
<root><item name="alpha">beta</item></root>
EOF
"$BIN_DIR/xmlmin" "$WORK_DIR/sample.xml" > "$WORK_DIR/xmlmin.out"
assert_file_contains "$WORK_DIR/xmlmin.out" '<root><item name="alpha">beta</item></root>' "inception xmlmin output mismatch"
"$BIN_DIR/xmltokens" "$WORK_DIR/sample.xml" > "$WORK_DIR/xmltokens.out"
assert_file_contains "$WORK_DIR/xmltokens.out" 'root' "inception xmltokens did not mention root"
"$BIN_DIR/xml2json" "$WORK_DIR/sample.xml" > "$WORK_DIR/xml2json.out"
assert_file_contains "$WORK_DIR/xml2json.out" 'root' "inception xml2json did not mention root"

echo "INCEPTION_SUITE_OK"