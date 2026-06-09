#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"
. "$ROOT_DIR/tests/lib/build.sh"

newos_configure_test_tools

WORK_DIR="$ROOT_DIR/tests/tmp/extended_tools"
HTTP_PORT_BASE=$((28000 + ($$ % 1000) * 10))
HTTP_PORT_STATIC=$HTTP_PORT_BASE
HTTP_PORT_CLI_CONFLICT=$((HTTP_PORT_BASE + 1))
HTTP_PORT_SERVICE=$((HTTP_PORT_BASE + 2))
HTTP_PORT_DIRECT_DROP=$((HTTP_PORT_BASE + 3))
HTTP_PORT_SERVICE_DROP=$((HTTP_PORT_BASE + 4))
HTTP_PORT_BAD_ID=$((HTTP_PORT_BASE + 5))
HTTP_PORT_LOG_SYMLINK=$((HTTP_PORT_BASE + 6))
HTTP_PORT_STALE_PIDFILE=$((HTTP_PORT_BASE + 7))
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

note "extended workflows"

mkdir -p "$WORK_DIR/rg_tree/src" "$WORK_DIR/rg_tree/.hidden"
printf 'alpha\nneedle one\n' > "$WORK_DIR/rg_tree/src/a.c"
printf 'NEEDLE two\n' > "$WORK_DIR/rg_tree/src/b.md"
printf 'needle hidden\n' > "$WORK_DIR/rg_tree/.hidden/secret.c"
"${TEST_BIN_DIR}/rg" needle "$WORK_DIR/rg_tree" > "$WORK_DIR/rg_basic.out"
assert_file_contains "$WORK_DIR/rg_basic.out" 'src/a\.c:2:needle one' "rg recursive search did not find a visible match"
if grep -q 'secret\.c' "$WORK_DIR/rg_basic.out"; then
    fail "rg should skip hidden files by default"
fi
"${TEST_BIN_DIR}/rg" --hidden needle "$WORK_DIR/rg_tree" > "$WORK_DIR/rg_hidden.out"
assert_file_contains "$WORK_DIR/rg_hidden.out" '\.hidden/secret\.c:1:needle hidden' "rg --hidden did not include hidden files"
"${TEST_BIN_DIR}/rg" -i -t md needle "$WORK_DIR/rg_tree" > "$WORK_DIR/rg_type.out"
assert_file_contains "$WORK_DIR/rg_type.out" 'src/b\.md:1:NEEDLE two' "rg -t md -i did not find the Markdown match"
"${TEST_BIN_DIR}/rg" --files -g '*.md' "$WORK_DIR/rg_tree" > "$WORK_DIR/rg_files.out"
assert_file_contains "$WORK_DIR/rg_files.out" 'src/b\.md$' "rg --files -g did not list the Markdown file"
mkdir -p "$WORK_DIR/rg_tree/ignored_dir" "$WORK_DIR/rg_tree/src/nested"
printf 'ignored needle\n' > "$WORK_DIR/rg_tree/ignored.log"
printf 'ignored dir needle\n' > "$WORK_DIR/rg_tree/ignored_dir/hidden.txt"
printf 'needle deep\n' > "$WORK_DIR/rg_tree/src/nested/deep.c"
printf 'ignored.log\nignored_dir/\n' > "$WORK_DIR/rg_tree/.gitignore"
"${TEST_BIN_DIR}/rg" needle "$WORK_DIR/rg_tree" > "$WORK_DIR/rg_ignore.out"
if grep -q 'ignored\.log\|ignored_dir' "$WORK_DIR/rg_ignore.out"; then
    fail "rg should honor .gitignore entries by default"
fi
"${TEST_BIN_DIR}/rg" --no-ignore needle "$WORK_DIR/rg_tree" > "$WORK_DIR/rg_no_ignore.out"
assert_file_contains "$WORK_DIR/rg_no_ignore.out" 'ignored\.log:1:ignored needle' "rg --no-ignore did not include ignored files"
"${TEST_BIN_DIR}/rg" -S needle "$WORK_DIR/rg_tree" > "$WORK_DIR/rg_smart_lower.out"
assert_file_contains "$WORK_DIR/rg_smart_lower.out" 'src/b\.md:1:NEEDLE two' "rg -S lowercase pattern should match case-insensitively"
"${TEST_BIN_DIR}/rg" -S NEEDLE "$WORK_DIR/rg_tree" > "$WORK_DIR/rg_smart_upper.out"
assert_file_contains "$WORK_DIR/rg_smart_upper.out" 'src/b\.md:1:NEEDLE two' "rg -S uppercase pattern should match case-sensitively"
if grep -q 'src/a\.c:2:needle one' "$WORK_DIR/rg_smart_upper.out"; then
    fail "rg -S uppercase pattern should not match lowercase text"
fi
"${TEST_BIN_DIR}/rg" -e alpha -e 'NEEDLE two' "$WORK_DIR/rg_tree" > "$WORK_DIR/rg_multi_pattern.out"
assert_file_contains "$WORK_DIR/rg_multi_pattern.out" 'src/a\.c:1:alpha' "rg -e did not match the first explicit pattern"
assert_file_contains "$WORK_DIR/rg_multi_pattern.out" 'src/b\.md:1:NEEDLE two' "rg -e did not match the second explicit pattern"
"${TEST_BIN_DIR}/rg" -o --column needle "$WORK_DIR/rg_tree/src/a.c" > "$WORK_DIR/rg_only_column.out"
assert_file_contains "$WORK_DIR/rg_only_column.out" '^2:1:needle$' "rg -o --column did not print the expected match span"
"${TEST_BIN_DIR}/rg" --files --sort path --include '*.c' --exclude 'deep.c' "$WORK_DIR/rg_tree" > "$WORK_DIR/rg_include_exclude.out"
assert_file_contains "$WORK_DIR/rg_include_exclude.out" 'src/a\.c$' "rg --include did not keep a matching C file"
if grep -q 'deep\.c' "$WORK_DIR/rg_include_exclude.out"; then
    fail "rg --exclude did not remove the excluded file"
fi
"${TEST_BIN_DIR}/rg" --files --type-not md "$WORK_DIR/rg_tree" > "$WORK_DIR/rg_type_not.out"
if grep -q 'b\.md' "$WORK_DIR/rg_type_not.out"; then
    fail "rg --type-not md should exclude Markdown files"
fi
"${TEST_BIN_DIR}/rg" --type-list > "$WORK_DIR/rg_type_list.out"
assert_file_contains "$WORK_DIR/rg_type_list.out" '^c: ' "rg --type-list did not print built-in types"
"${TEST_BIN_DIR}/rg" -m 1 needle "$WORK_DIR/rg_tree/src/nested/deep.c" > "$WORK_DIR/rg_max_count.out"
assert_file_contains "$WORK_DIR/rg_max_count.out" '^1:needle deep$' "rg -m did not keep the first matching line"

printf 'streamed-data\n' | "${TEST_BIN_DIR}/gzip" -c | "${TEST_BIN_DIR}/gunzip" -c > "$WORK_DIR/gzip_stream.txt"
assert_file_contains "$WORK_DIR/gzip_stream.txt" '^streamed-data$' "gzip/gunzip streaming pipeline failed"

"${TEST_BIN_DIR}/file" "${TEST_BIN_DIR}/echo" > "$WORK_DIR/echo_file.out"
if grep -q 'ELF' "$WORK_DIR/echo_file.out"; then
    "${TEST_BIN_DIR}/expack" -q "${TEST_BIN_DIR}/echo" "$WORK_DIR/echo.packed"
    "$WORK_DIR/echo.packed" expack-ok > "$WORK_DIR/expack.out"
    assert_file_contains "$WORK_DIR/expack.out" '^expack-ok$' "expack output did not preserve executable behavior"
    "${TEST_BIN_DIR}/file" "$WORK_DIR/echo.packed" > "$WORK_DIR/expack_file.out"
    assert_file_contains "$WORK_DIR/expack_file.out" 'ELF' "expack output is not recognized as ELF"
else
    if "${TEST_BIN_DIR}/expack" --analyze "${TEST_BIN_DIR}/echo" > "$WORK_DIR/expack_host.out" 2> "$WORK_DIR/expack_host.err"; then
        assert_file_contains "$WORK_DIR/expack_host.out" 'format Mach-O' "expack did not analyze the Mach-O host executable"
        assert_file_contains "$WORK_DIR/expack_host.out" '^selected: ' "expack did not select a Mach-O compression candidate"
        "${TEST_BIN_DIR}/expack" "${TEST_BIN_DIR}/echo" "$WORK_DIR/expack_host.container" > "$WORK_DIR/expack_host_container.out"
        assert_file_contains "$WORK_DIR/expack_host_container.out" '^  lzrep: payload ' "expack did not report runnable Mach-O compression candidates while packing"
        assert_file_contains "$WORK_DIR/expack_host_container.out" 'wrote Mach-O prototype container' "expack did not report Mach-O container output"
        assert_file_contains "$WORK_DIR/expack_host_container.out" 'output .* bytes, payload ' "expack did not report Mach-O container output statistics"
        "${TEST_BIN_DIR}/file" "$WORK_DIR/expack_host.container" > "$WORK_DIR/expack_host_container_file.out"
        assert_file_contains "$WORK_DIR/expack_host_container_file.out" 'Mach-O' "expack Mach-O container is not recognized as Mach-O"
        if [ "$(uname -s 2>/dev/null || echo unknown)" = Darwin ] && command -v codesign >/dev/null 2>&1; then
            codesign --verify --strict --verbose=4 "$WORK_DIR/expack_host.container" > "$WORK_DIR/expack_host_container_verify.out" 2>&1
            codesign -dv "$WORK_DIR/expack_host.container" > "$WORK_DIR/expack_host_container_codesign.out" 2>&1
            assert_file_contains "$WORK_DIR/expack_host_container_codesign.out" 'Signature=adhoc' "expack did not emit an ad-hoc signature for the Mach-O host container"
            if [ "$(uname -m 2>/dev/null || echo unknown)" = arm64 ]; then
                "$WORK_DIR/expack_host.container" expack-ok > "$WORK_DIR/expack_host_container_run.out"
                assert_file_contains "$WORK_DIR/expack_host_container_run.out" '^expack-ok$' "expack Mach-O container did not preserve executable behavior"
            fi
        fi
    else
        assert_file_contains "$WORK_DIR/expack_host.err" 'recognized, but expack does not have' "expack did not identify the unsupported host executable format"
    fi
fi

if [ "$(uname -s 2>/dev/null || echo unknown)" = Darwin ] && [ -x "${TEST_BIN_DIR}/expack" ]; then
    "${TEST_BIN_DIR}/expack" --analyze "${TEST_BIN_DIR}/expack" > "$WORK_DIR/expack_freestanding_macho.out"
    assert_file_contains "$WORK_DIR/expack_freestanding_macho.out" 'format Mach-O' "freestanding expack did not analyze its own Mach-O executable"
    assert_file_contains "$WORK_DIR/expack_freestanding_macho.out" '^selected: ' "freestanding expack did not select a Mach-O compression candidate"
fi

if [ "$(uname -s 2>/dev/null || echo unknown)" = Linux ] && [ "$(uname -m 2>/dev/null || echo unknown)" = x86_64 ] && [ -x "${TEST_BIN_DIR}/expack" ] && [ -x "${TEST_BIN_DIR}/echo" ]; then
    "${TEST_BIN_DIR}/expack" --analyze "${TEST_BIN_DIR}/echo" > "$WORK_DIR/expack_freestanding_linux_analyze.out"
    assert_file_contains "$WORK_DIR/expack_freestanding_linux_analyze.out" '^  lzss-bcj/medium-match: payload ' "expack did not evaluate the normal BCJ medium-match candidate for Linux ELF"
    if grep -q '^  lzss-bcj/long-match: payload ' "$WORK_DIR/expack_freestanding_linux_analyze.out"; then
        fail "expack should not evaluate BCJ long-match in the normal Linux ELF portfolio"
    fi
    if grep -q '^  lz4-block: payload ' "$WORK_DIR/expack_freestanding_linux_analyze.out"; then
        fail "expack should not evaluate LZ4-block in the normal Linux ELF portfolio"
    fi
    "${TEST_BIN_DIR}/expack" --analyze --all "${TEST_BIN_DIR}/echo" > "$WORK_DIR/expack_freestanding_linux_all.out"
    assert_file_contains "$WORK_DIR/expack_freestanding_linux_all.out" '^  lz4-block: payload ' "expack --all did not evaluate the LZ4-block codec for Linux ELF"
    assert_file_contains "$WORK_DIR/expack_freestanding_linux_all.out" '^  xlz-short: payload ' "expack --all did not evaluate the experimental XLZ codec for Linux ELF"
    assert_file_contains "$WORK_DIR/expack_freestanding_linux_all.out" '^  lzss-bcj-rip/long-match: payload ' "expack --all did not evaluate BCJ-RIP with all LZSS profiles for Linux ELF"
    "${TEST_BIN_DIR}/expack" --all -q "${TEST_BIN_DIR}/true" "$WORK_DIR/true.freestanding-linux-all.packed"
    "$WORK_DIR/true.freestanding-linux-all.packed"
    "${TEST_BIN_DIR}/expack" -q "${TEST_BIN_DIR}/echo" "$WORK_DIR/echo.freestanding-linux.packed"
    "$WORK_DIR/echo.freestanding-linux.packed" expack-linux-ok > "$WORK_DIR/expack_freestanding_linux.out"
    assert_file_contains "$WORK_DIR/expack_freestanding_linux.out" '^expack-linux-ok$' "freestanding Linux expack output did not preserve executable behavior"
    "${TEST_BIN_DIR}/file" "$WORK_DIR/echo.freestanding-linux.packed" > "$WORK_DIR/expack_freestanding_linux_file.out"
    assert_file_contains "$WORK_DIR/expack_freestanding_linux_file.out" 'ELF' "freestanding Linux expack output is not recognized as ELF"
fi

"${TEST_BIN_DIR}/portscan" --profile admin --summary 127.0.0.1 > "$WORK_DIR/portscan_profile.out"
assert_file_contains "$WORK_DIR/portscan_profile.out" 'summary scanned=11 ' "portscan profile summary did not scan the admin profile"
assert_file_contains "$WORK_DIR/portscan_profile.out" ' jobs=1 timeout_ms=1000' "portscan summary did not include scan parameters"

cat > "$WORK_DIR/portscan_baseline.csv" <<'EOF'
host,port,state,service
127.0.0.1,1,open,old
EOF
"${TEST_BIN_DIR}/portscan" --csv --baseline "$WORK_DIR/portscan_baseline.csv" --diff -a 127.0.0.1 1 > "$WORK_DIR/portscan_diff.csv"
assert_file_contains "$WORK_DIR/portscan_diff.csv" '^host,port,state,service,latency_ms,reason,change,banner,tls_protocol' "portscan CSV header did not include detailed columns"
assert_file_contains "$WORK_DIR/portscan_diff.csv" '^127\.0\.0\.1,1,closed,,[0-9][0-9]*,connection_refused,now-closed,' "portscan baseline diff did not report a now-closed port"

"${TEST_BIN_DIR}/portscan" --json --summary --jobs 2 --per-host 1 -a '[127.0.0.1]' 1,2 > "$WORK_DIR/portscan_json.out"
assert_file_contains "$WORK_DIR/portscan_json.out" '"latency_ms":' "portscan JSON result did not include latency"
assert_file_contains "$WORK_DIR/portscan_json.out" '"reason":"connection_refused"' "portscan JSON result did not include reason detail"
assert_file_contains "$WORK_DIR/portscan_json.out" '"event":"scan_summary"' "portscan JSON summary was not emitted"

if [ "$(uname -s 2>/dev/null || echo unknown)" = Linux ] && [ -x "${TEST_BIN_DIR}/portscan" ] && command -v openssl >/dev/null 2>&1; then
    TLS_PORT=$((33000 + ($$ % 1000)))
    openssl req -x509 -newkey rsa:2048 -nodes -subj '/CN=localhost/O=newos-test' \
        -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' -days 1 \
        -keyout "$WORK_DIR/portscan.key" -out "$WORK_DIR/portscan.crt" > "$WORK_DIR/portscan_openssl_req.out" 2>&1
    openssl s_server -quiet -accept "$TLS_PORT" -cert "$WORK_DIR/portscan.crt" -key "$WORK_DIR/portscan.key" > "$WORK_DIR/portscan_sserver.out" 2>&1 &
    TLS_PID=$!
    sleep 1
    "${TEST_BIN_DIR}/portscan" --tls-cert --tls-insecure --details 127.0.0.1 "$TLS_PORT" > "$WORK_DIR/portscan_tls.out"
    kill "$TLS_PID" 2>/dev/null || true
    assert_file_contains "$WORK_DIR/portscan_tls.out" 'tls=TLSv1\.[23]' "portscan TLS certificate mode did not report a TLS protocol"
    assert_file_contains "$WORK_DIR/portscan_tls.out" 'cert_subject=CN=localhost,O=newos-test' "portscan TLS certificate mode did not report certificate subject"
fi

mkdir -p "$WORK_DIR/tar_src"
printf 'archive-data\n' > "$WORK_DIR/tar_src/file.txt"
(
    cd "$WORK_DIR"
    "${TEST_BIN_DIR}/tar" -czf test.tar.gz tar_src
    rm -rf tar_src
    mkdir -p tar_extract
    cd tar_extract
    "${TEST_BIN_DIR}/tar" -xzf ../test.tar.gz
)
assert_file_contains "$WORK_DIR/tar_extract/tar_src/file.txt" '^archive-data$' "tar compressed archive round-trip failed"

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
    "${TEST_BIN_DIR}/patch" -p1 --dry-run -i patch.diff > patch_dry.out
)
assert_file_contains "$WORK_DIR/patch_dry.out" '^checked ' "patch --dry-run did not validate the patch input"
assert_file_contains "$WORK_DIR/patch_target.txt" '^beta$' "patch --dry-run unexpectedly modified the source file"
(
    cd "$WORK_DIR"
    cat patch.diff | "${TEST_BIN_DIR}/patch" -p1
)
patch_status=0
(
    cd "$WORK_DIR"
    "${TEST_BIN_DIR}/patch" -p1 -i patch.diff
) > "$WORK_DIR/patch_repeat.out" 2>&1 || patch_status=$?
assert_text_equals "$patch_status" '1' "patch should reject an already-applied hunk"
assert_file_contains "$WORK_DIR/patch_repeat.out" 'already applied' "patch did not explain the repeated-apply failure"
(
    cd "$WORK_DIR"
    "${TEST_BIN_DIR}/patch" -R -p1 -i patch.diff
)
assert_file_contains "$WORK_DIR/patch_target.txt" '^beta$' "patch reverse apply failed"
(
    cd "$WORK_DIR"
    "${TEST_BIN_DIR}/patch" -p1 -o patch_preview.txt -i patch.diff
)
assert_file_contains "$WORK_DIR/patch_preview.txt" '^gamma$' "patch -o did not write the patched preview output"

printf 'outside\n' > "$WORK_DIR/outside.txt"
mkdir -p "$WORK_DIR/patch_guard"
cat > "$WORK_DIR/unsafe.patch" <<'EOF'
--- a/../outside.txt
+++ b/../outside.txt
@@ -1 +1 @@
-outside
+owned
EOF
patch_unsafe_status=0
(
    cd "$WORK_DIR/patch_guard"
    "${TEST_BIN_DIR}/patch" -p1 -i "$WORK_DIR/unsafe.patch"
) > "$WORK_DIR/patch_unsafe.out" 2>&1 || patch_unsafe_status=$?
if [ "$patch_unsafe_status" -eq 0 ]; then
    fail "patch should refuse a parent-traversing target path"
fi
assert_file_contains "$WORK_DIR/patch_unsafe.out" 'refusing unsafe path' "patch did not explain the unsafe-path refusal"
assert_file_contains "$WORK_DIR/outside.txt" '^outside$' "patch modified a file outside the working tree"

printf 'alpha\nbeta\nGamma\n' > "$WORK_DIR/pager.txt"
"${TEST_BIN_DIR}/less" -N -p gamma "$WORK_DIR/pager.txt" > "$WORK_DIR/less_search.out"
assert_file_contains "$WORK_DIR/less_search.out" '^3[[:space:]][[:space:]]*Gamma$' "less -p did not jump to the requested match"
if grep -q '^1[[:space:]][[:space:]]*alpha$' "$WORK_DIR/less_search.out"; then
    fail "less -p unexpectedly printed content before the requested match"
fi

"${TEST_BIN_DIR}/more" -N +/beta "$WORK_DIR/pager.txt" > "$WORK_DIR/more_search.out"
assert_file_contains "$WORK_DIR/more_search.out" '^2[[:space:]][[:space:]]*beta$' "more +/pattern did not jump to the first matching line"
assert_file_contains "$WORK_DIR/more_search.out" '^3[[:space:]][[:space:]]*Gamma$' "more search output was incomplete"

"${TEST_BIN_DIR}/more" --color=always -N "$WORK_DIR/pager.txt" > "$WORK_DIR/more_color.out"
if ! LC_ALL=C grep -q "$(printf '\033')\\[" "$WORK_DIR/more_color.out"; then
    fail "more --color=always did not emit ANSI color sequences"
fi

mkdir -p "$WORK_DIR/make_include"
cat > "$WORK_DIR/make_include/Makefile" <<'EOF'
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
    "${TEST_BIN_DIR}/make"
)
assert_file_contains "$WORK_DIR/make_include/built.txt" '^included value$' "make include handling failed"

mkdir -p "$WORK_DIR/make_pattern"
cat > "$WORK_DIR/make_pattern/Makefile" <<'EOF'
all: result.txt

%.txt: %.in
	printf 'stem=%s src=%s\n' "$*" "$<" > "$@"
EOF
printf 'pattern-input\n' > "$WORK_DIR/make_pattern/result.in"
(
    cd "$WORK_DIR/make_pattern"
    "${TEST_BIN_DIR}/make"
)
assert_file_contains "$WORK_DIR/make_pattern/result.txt" '^stem=result src=result.in$' "make pattern rule or automatic variables failed"

mkdir -p "$WORK_DIR/make_gnuish"
cat > "$WORK_DIR/make_gnuish/Makefile" <<'EOF'
ifeq ($(origin MODE), undefined)
MODE := default
endif

LIST := \
    alpha \
    beta
FILTERED := $(filter beta,$(LIST))
PREFIXED := $(addprefix x-,$(LIST))
STRIPPED := $(strip   $(FILTERED)   )
STAMP := $(shell printf shell-ok)

ifeq ($(STRIPPED),beta)
RESULT := $(STAMP) $(PREFIXED)
else
RESULT := bad
endif

all:
	printf '%s\n' "$(RESULT)" > out.txt
EOF
(
    cd "$WORK_DIR/make_gnuish"
    "${TEST_BIN_DIR}/make"
)
assert_file_contains "$WORK_DIR/make_gnuish/out.txt" '^shell-ok x-alpha x-beta$' "make did not handle repository-style functions and conditionals"

"${TEST_BIN_DIR}/netcat" -l 24681 > "$WORK_DIR/netcat_server.out" &
netcat_pid=$!
"${TEST_BIN_DIR}/sleep" 1
printf 'hello nc\n' | "${TEST_BIN_DIR}/netcat" localhost 24681 > "$WORK_DIR/netcat_client.out"
wait "$netcat_pid"
assert_file_contains "$WORK_DIR/netcat_server.out" 'hello nc' "netcat listener did not receive the TCP payload"

"${TEST_BIN_DIR}/netcat" -u -l -w 3 24682 > "$WORK_DIR/netcat_udp_server.out" &
netcat_udp_pid=$!
"${TEST_BIN_DIR}/sleep" 1
printf 'hello udp\n' | "${TEST_BIN_DIR}/netcat" -u -w 1 localhost 24682 > "$WORK_DIR/netcat_udp_client.out"
wait "$netcat_udp_pid"
assert_file_contains "$WORK_DIR/netcat_udp_server.out" 'hello udp' "netcat UDP mode did not receive the payload"

"${TEST_BIN_DIR}/netcat" -4 -k -l -s 127.0.0.1 -w 1500ms 24683 > "$WORK_DIR/netcat_keep_server.out" &
netcat_keep_pid=$!
"${TEST_BIN_DIR}/sleep" 1
printf 'hello keep one\n' | "${TEST_BIN_DIR}/netcat" -4 -n 127.0.0.1 24683 > "$WORK_DIR/netcat_keep_client1.out"
printf 'hello keep two\n' | "${TEST_BIN_DIR}/netcat" -4 -n 127.0.0.1 24683 > "$WORK_DIR/netcat_keep_client2.out"
wait "$netcat_keep_pid"
assert_file_contains "$WORK_DIR/netcat_keep_server.out" 'hello keep one' "netcat -k did not keep the listener alive for the first payload"
assert_file_contains "$WORK_DIR/netcat_keep_server.out" 'hello keep two' "netcat -k did not keep the listener alive for the second payload"

"${TEST_BIN_DIR}/shutdown" --help > "$WORK_DIR/shutdown_help.out" 2>&1
assert_file_contains "$WORK_DIR/shutdown_help.out" 'shutdown' "shutdown help output missing"

mkdir -p "$WORK_DIR/http_root"
printf 'hello from httpd\n' > "$WORK_DIR/http_root/index.txt"
printf 'hidden\n' > "$WORK_DIR/http_root/.secret"
printf 'outside root\n' > "$WORK_DIR/outside_root.txt"
ln "$WORK_DIR/outside_root.txt" "$WORK_DIR/http_root/hardlink.txt"
"${TEST_BIN_DIR}/httpd" -p "$HTTP_PORT_STATIC" -r "$WORK_DIR/http_root" > "$WORK_DIR/httpd.log" 2>&1 &
httpd_pid=$!
trap 'kill "$httpd_pid" 2>/dev/null || true' EXIT INT TERM
"${TEST_BIN_DIR}/sleep" 1
"${TEST_BIN_DIR}/wget" -q -O "$WORK_DIR/http_fetch.txt" "http://127.0.0.1:$HTTP_PORT_STATIC/index.txt"
assert_file_contains "$WORK_DIR/http_fetch.txt" '^hello from httpd$' "httpd did not serve the requested static file"
mkdir -p "$WORK_DIR/http_root/api/rest_v1/page/summary"
cat > "$WORK_DIR/http_root/api/rest_v1/page/summary/Wrap_Test" <<'EOF'
{"title":"Wrap Test","description":"fixture","extract":"alpha beta gamma delta epsilon zeta"}
EOF
COLUMNS=24 "${TEST_BIN_DIR}/wtf" --base-url "http://127.0.0.1:$HTTP_PORT_STATIC/api/rest_v1/page/summary" --only-extract Wrap Test > "$WORK_DIR/wtf_wrap.out"
assert_file_contains "$WORK_DIR/wtf_wrap.out" '^alpha beta gamma delta$' "wtf should wrap extract text at a word boundary"
assert_file_contains "$WORK_DIR/wtf_wrap.out" '^epsilon zeta$' "wtf should carry remaining extract words to the next line"
http_hidden_status=0
"${TEST_BIN_DIR}/wget" -q -O "$WORK_DIR/http_hidden.txt" "http://127.0.0.1:$HTTP_PORT_STATIC/.secret" > "$WORK_DIR/http_hidden.out" 2>&1 || http_hidden_status=$?
if [ "$http_hidden_status" -eq 0 ]; then
    fail "httpd should refuse to serve dotfiles from the document root"
fi
http_link_status=0
"${TEST_BIN_DIR}/wget" -q -O "$WORK_DIR/http_hardlink.txt" "http://127.0.0.1:$HTTP_PORT_STATIC/hardlink.txt" > "$WORK_DIR/http_hardlink.out" 2>&1 || http_link_status=$?
if [ "$http_link_status" -eq 0 ]; then
    fail "httpd should refuse multiply linked files from the document root"
fi
printf 'GET /index.txt\r\n\r\n' | "${TEST_BIN_DIR}/netcat" -4 -n -w 1 127.0.0.1 "$HTTP_PORT_STATIC" > "$WORK_DIR/http_bad_request.out"
assert_file_contains "$WORK_DIR/http_bad_request.out" '^HTTP/1\.1 400 Bad Request' "httpd should reject malformed requests without an HTTP version"
printf 'GET /index.txt HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\nTEST' | "${TEST_BIN_DIR}/netcat" -4 -n -w 1 127.0.0.1 "$HTTP_PORT_STATIC" > "$WORK_DIR/http_body_header.out"
assert_file_contains "$WORK_DIR/http_body_header.out" '^HTTP/1\.1 400 Bad Request' "httpd should reject GET requests with a request body framing header"
mkfifo "$WORK_DIR/http_idle_pipe"
"${TEST_BIN_DIR}/netcat" -4 -n 127.0.0.1 "$HTTP_PORT_STATIC" < "$WORK_DIR/http_idle_pipe" > "$WORK_DIR/http_idle_hold.out" 2>&1 &
httpd_idle_pid=$!
exec 9> "$WORK_DIR/http_idle_pipe"
"${TEST_BIN_DIR}/sleep" 1
http_idle_status=0
"${TEST_BIN_DIR}/timeout" 3s "${TEST_BIN_DIR}/wget" -q -O "$WORK_DIR/http_idle_fetch.txt" "http://127.0.0.1:$HTTP_PORT_STATIC/index.txt" > "$WORK_DIR/http_idle_fetch.out" 2>&1 || http_idle_status=$?
exec 9>&-
wait "$httpd_idle_pid" 2>/dev/null || true
if [ "$http_idle_status" -ne 0 ]; then
    fail "httpd should continue serving new clients while another connection stays idle"
fi
assert_file_contains "$WORK_DIR/http_idle_fetch.txt" '^hello from httpd$' "httpd should still serve static files while an idle client is connected"
kill "$httpd_pid" 2>/dev/null || true
wait "$httpd_pid" 2>/dev/null || true
trap - EXIT INT TERM

cat > "$WORK_DIR/httpd_cli_conflict.conf" <<EOF
bind=127.0.0.1
port=$HTTP_PORT_CLI_CONFLICT
root=$WORK_DIR/http_root
EOF
http_conflict_rc=0
"${TEST_BIN_DIR}/timeout" 1s "${TEST_BIN_DIR}/httpd" -p "$HTTP_PORT_SERVICE" -c "$WORK_DIR/httpd_cli_conflict.conf" > "$WORK_DIR/httpd_cli_conflict.out" 2>&1 || http_conflict_rc=$?
if [ "$http_conflict_rc" -eq 0 ] || [ "$http_conflict_rc" -eq 124 ]; then
    fail "httpd should refuse conflicting CLI and config values rather than silently choosing one"
fi

cat > "$WORK_DIR/httpd.conf" <<EOF
command=${TEST_BIN_DIR}/httpd -p $HTTP_PORT_SERVICE -r $WORK_DIR/http_root
pidfile=$WORK_DIR/httpd.pid
stdout=$WORK_DIR/service-httpd.out
stderr=$WORK_DIR/service-httpd.err
EOF
"${TEST_BIN_DIR}/service" start "$WORK_DIR/httpd.conf" > "$WORK_DIR/service_start.out"
"${TEST_BIN_DIR}/sleep" 1
"${TEST_BIN_DIR}/service" status "$WORK_DIR/httpd.conf" > "$WORK_DIR/service_status.out"
assert_file_contains "$WORK_DIR/service_status.out" 'running' "service status did not report the daemon as running"
"${TEST_BIN_DIR}/wget" -q -O "$WORK_DIR/http_service_fetch.txt" "http://127.0.0.1:$HTTP_PORT_SERVICE/index.txt"
assert_file_contains "$WORK_DIR/http_service_fetch.txt" '^hello from httpd$' "service-managed httpd did not serve the requested static file"
cat > "$WORK_DIR/httpd_conflict.conf" <<EOF
command=${TEST_BIN_DIR}/httpd -p $HTTP_PORT_SERVICE -r $WORK_DIR/http_root
pidfile=$WORK_DIR/httpd-conflict.pid
stdout=$WORK_DIR/service-httpd-conflict.out
stderr=$WORK_DIR/service-httpd-conflict.err
EOF
service_conflict_status=0
"${TEST_BIN_DIR}/service" start "$WORK_DIR/httpd_conflict.conf" > "$WORK_DIR/service_conflict_start.out" 2>&1 || service_conflict_status=$?
if [ "$service_conflict_status" -eq 0 ]; then
    fail "service should report a startup failure when the requested port is already in use"
fi
"${TEST_BIN_DIR}/service" stop "$WORK_DIR/httpd.conf" > "$WORK_DIR/service_stop.out"
assert_file_contains "$WORK_DIR/service_stop.out" 'stopped' "service stop did not report a clean shutdown"

service_user=$(id -un 2>/dev/null || whoami)
service_group=$(id -gn 2>/dev/null || echo staff)
cat > "$WORK_DIR/httpd_direct_drop.conf" <<EOF
bind=127.0.0.1
port=$HTTP_PORT_DIRECT_DROP
root=$WORK_DIR/http_root
index=index.txt
user=$service_user
group=$service_group
EOF
"${TEST_BIN_DIR}/httpd" -c "$WORK_DIR/httpd_direct_drop.conf" > "$WORK_DIR/httpd_direct_drop.log" 2>&1 &
httpd_drop_pid=$!
trap 'kill "$httpd_drop_pid" 2>/dev/null || true' EXIT INT TERM
"${TEST_BIN_DIR}/sleep" 1
"${TEST_BIN_DIR}/wget" -q -O "$WORK_DIR/http_direct_drop_fetch.txt" "http://127.0.0.1:$HTTP_PORT_DIRECT_DROP/index.txt"
assert_file_contains "$WORK_DIR/http_direct_drop_fetch.txt" '^hello from httpd$' "httpd with configured post-bind privilege drop did not serve the requested static file"
kill "$httpd_drop_pid" 2>/dev/null || true
wait "$httpd_drop_pid" 2>/dev/null || true
trap - EXIT INT TERM

cat > "$WORK_DIR/httpd_drop.conf" <<EOF
command=${TEST_BIN_DIR}/httpd -p $HTTP_PORT_SERVICE_DROP -r $WORK_DIR/http_root
pidfile=$WORK_DIR/httpd-drop.pid
stdout=$WORK_DIR/service-httpd-drop.out
stderr=$WORK_DIR/service-httpd-drop.err
user=$service_user
group=$service_group
EOF
"${TEST_BIN_DIR}/service" start "$WORK_DIR/httpd_drop.conf" > "$WORK_DIR/service_drop_start.out"
"${TEST_BIN_DIR}/sleep" 1
"${TEST_BIN_DIR}/wget" -q -O "$WORK_DIR/http_drop_fetch.txt" "http://127.0.0.1:$HTTP_PORT_SERVICE_DROP/index.txt"
assert_file_contains "$WORK_DIR/http_drop_fetch.txt" '^hello from httpd$' "service-managed httpd with configured privilege drop did not serve the requested static file"
"${TEST_BIN_DIR}/service" stop "$WORK_DIR/httpd_drop.conf" > "$WORK_DIR/service_drop_stop.out"

cat > "$WORK_DIR/httpd_bad_identity.conf" <<EOF
command=${TEST_BIN_DIR}/httpd -p $HTTP_PORT_BAD_ID -r $WORK_DIR/http_root
pidfile=$WORK_DIR/httpd-bad-identity.pid
stdout=$WORK_DIR/service-httpd-bad-identity.out
stderr=$WORK_DIR/service-httpd-bad-identity.err
user=this-user-should-not-exist-newos
EOF
bad_identity_status=0
"${TEST_BIN_DIR}/service" start "$WORK_DIR/httpd_bad_identity.conf" > "$WORK_DIR/service_bad_identity.out" 2>&1 || bad_identity_status=$?
if [ "$bad_identity_status" -eq 0 ]; then
    fail "service should reject an invalid target identity for privilege dropping"
fi

mkdir -p "$WORK_DIR/bin"
cat > "$WORK_DIR/bin/fake-httpd" <<'EOF'
#!/bin/sh
printf 'unsafe path search executed\n' > "$0.ran"
while :; do sleep 1; done
EOF
chmod +x "$WORK_DIR/bin/fake-httpd"
cat > "$WORK_DIR/unsafe_service.conf" <<EOF
command=fake-httpd
pidfile=$WORK_DIR/unsafe.pid
stdout=$WORK_DIR/unsafe.log
stderr=$WORK_DIR/unsafe.log
EOF
mkdir -p "$WORK_DIR/service_quote"
cat > "$WORK_DIR/quoted_service.conf" <<EOF
command=/bin/sh -c 'printf "%s\\n" "it'\''s ok" > "$WORK_DIR/service_quote/quoted.out"; while :; do sleep 1; done'
pidfile=$WORK_DIR/quoted.pid
stdout=$WORK_DIR/quoted.log
stderr=$WORK_DIR/quoted.log
EOF
"${TEST_BIN_DIR}/service" start "$WORK_DIR/quoted_service.conf" > "$WORK_DIR/quoted_service_start.out"
"${TEST_BIN_DIR}/sleep" 1
assert_file_contains "$WORK_DIR/service_quote/quoted.out" "^it's ok$" "service command parsing should preserve standard single-quote shell escaping"
"${TEST_BIN_DIR}/service" stop "$WORK_DIR/quoted_service.conf" > "$WORK_DIR/quoted_service_stop.out"

ln -sf "$WORK_DIR/real_service.log" "$WORK_DIR/symlink_service.log"
cat > "$WORK_DIR/symlink_log_service.conf" <<EOF
command=${TEST_BIN_DIR}/httpd -p $HTTP_PORT_LOG_SYMLINK -r $WORK_DIR/http_root
pidfile=$WORK_DIR/symlink-log.pid
stdout=$WORK_DIR/symlink_service.log
stderr=$WORK_DIR/symlink_service.log
EOF
symlink_log_status=0
"${TEST_BIN_DIR}/service" start "$WORK_DIR/symlink_log_service.conf" > "$WORK_DIR/symlink_log_service.out" 2>&1 || symlink_log_status=$?
if [ "$symlink_log_status" -eq 0 ]; then
    "${TEST_BIN_DIR}/service" stop "$WORK_DIR/symlink_log_service.conf" > /dev/null 2>&1 || true
    fail "service should reject symlinked log output targets"
fi

"${TEST_BIN_DIR}/sleep" 30 &
stray_pid=$!
printf '%s\n' "$stray_pid" > "$WORK_DIR/stray.pid"
cat > "$WORK_DIR/stray_service.conf" <<EOF
command=${TEST_BIN_DIR}/httpd -p $HTTP_PORT_STALE_PIDFILE -r $WORK_DIR/http_root
pidfile=$WORK_DIR/stray.pid
stdout=$WORK_DIR/stray.log
stderr=$WORK_DIR/stray.log
EOF
stray_stop_status=0
"${TEST_BIN_DIR}/service" stop "$WORK_DIR/stray_service.conf" > "$WORK_DIR/stray_stop.out" 2>&1 || stray_stop_status=$?
if ! kill -0 "$stray_pid" 2>/dev/null; then
    fail "service should not kill an unrelated process referenced only by a stale pidfile"
fi
kill "$stray_pid" 2>/dev/null || true
wait "$stray_pid" 2>/dev/null || true

unsafe_status=0
PATH="$WORK_DIR/bin:$PATH" "${TEST_BIN_DIR}/service" start "$WORK_DIR/unsafe_service.conf" > "$WORK_DIR/unsafe_service.out" 2>&1 || unsafe_status=$?
if [ "$unsafe_status" -eq 0 ]; then
    fail "service should reject PATH-only command execution for security reasons"
fi
