#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"

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
"$ROOT_DIR/build/rg" needle "$WORK_DIR/rg_tree" > "$WORK_DIR/rg_basic.out"
assert_file_contains "$WORK_DIR/rg_basic.out" 'src/a\.c:2:needle one' "rg recursive search did not find a visible match"
if grep -q 'secret\.c' "$WORK_DIR/rg_basic.out"; then
    fail "rg should skip hidden files by default"
fi
"$ROOT_DIR/build/rg" --hidden needle "$WORK_DIR/rg_tree" > "$WORK_DIR/rg_hidden.out"
assert_file_contains "$WORK_DIR/rg_hidden.out" '\.hidden/secret\.c:1:needle hidden' "rg --hidden did not include hidden files"
"$ROOT_DIR/build/rg" -i -t md needle "$WORK_DIR/rg_tree" > "$WORK_DIR/rg_type.out"
assert_file_contains "$WORK_DIR/rg_type.out" 'src/b\.md:1:NEEDLE two' "rg -t md -i did not find the Markdown match"
"$ROOT_DIR/build/rg" --files -g '*.md' "$WORK_DIR/rg_tree" > "$WORK_DIR/rg_files.out"
assert_file_contains "$WORK_DIR/rg_files.out" 'src/b\.md$' "rg --files -g did not list the Markdown file"

printf 'streamed-data\n' | "$ROOT_DIR/build/gzip" -c | "$ROOT_DIR/build/gunzip" -c > "$WORK_DIR/gzip_stream.txt"
assert_file_contains "$WORK_DIR/gzip_stream.txt" '^streamed-data$' "gzip/gunzip streaming pipeline failed"

mkdir -p "$WORK_DIR/tar_src"
printf 'archive-data\n' > "$WORK_DIR/tar_src/file.txt"
(
    cd "$WORK_DIR"
    "$ROOT_DIR/build/tar" -czf test.tar.gz tar_src
    rm -rf tar_src
    mkdir -p tar_extract
    cd tar_extract
    "$ROOT_DIR/build/tar" -xzf ../test.tar.gz
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
    "$ROOT_DIR/build/patch" -p1 --dry-run -i patch.diff > patch_dry.out
)
assert_file_contains "$WORK_DIR/patch_dry.out" '^checked ' "patch --dry-run did not validate the patch input"
assert_file_contains "$WORK_DIR/patch_target.txt" '^beta$' "patch --dry-run unexpectedly modified the source file"
(
    cd "$WORK_DIR"
    cat patch.diff | "$ROOT_DIR/build/patch" -p1
)
patch_status=0
(
    cd "$WORK_DIR"
    "$ROOT_DIR/build/patch" -p1 -i patch.diff
) > "$WORK_DIR/patch_repeat.out" 2>&1 || patch_status=$?
assert_text_equals "$patch_status" '1' "patch should reject an already-applied hunk"
assert_file_contains "$WORK_DIR/patch_repeat.out" 'already applied' "patch did not explain the repeated-apply failure"
(
    cd "$WORK_DIR"
    "$ROOT_DIR/build/patch" -R -p1 -i patch.diff
)
assert_file_contains "$WORK_DIR/patch_target.txt" '^beta$' "patch reverse apply failed"
(
    cd "$WORK_DIR"
    "$ROOT_DIR/build/patch" -p1 -o patch_preview.txt -i patch.diff
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
    "$ROOT_DIR/build/patch" -p1 -i "$WORK_DIR/unsafe.patch"
) > "$WORK_DIR/patch_unsafe.out" 2>&1 || patch_unsafe_status=$?
if [ "$patch_unsafe_status" -eq 0 ]; then
    fail "patch should refuse a parent-traversing target path"
fi
assert_file_contains "$WORK_DIR/patch_unsafe.out" 'refusing unsafe path' "patch did not explain the unsafe-path refusal"
assert_file_contains "$WORK_DIR/outside.txt" '^outside$' "patch modified a file outside the working tree"

printf 'alpha\nbeta\nGamma\n' > "$WORK_DIR/pager.txt"
"$ROOT_DIR/build/less" -N -p gamma "$WORK_DIR/pager.txt" > "$WORK_DIR/less_search.out"
assert_file_contains "$WORK_DIR/less_search.out" '^3[[:space:]][[:space:]]*Gamma$' "less -p did not jump to the requested match"
if grep -q '^1[[:space:]][[:space:]]*alpha$' "$WORK_DIR/less_search.out"; then
    fail "less -p unexpectedly printed content before the requested match"
fi

"$ROOT_DIR/build/more" -N +/beta "$WORK_DIR/pager.txt" > "$WORK_DIR/more_search.out"
assert_file_contains "$WORK_DIR/more_search.out" '^2[[:space:]][[:space:]]*beta$' "more +/pattern did not jump to the first matching line"
assert_file_contains "$WORK_DIR/more_search.out" '^3[[:space:]][[:space:]]*Gamma$' "more search output was incomplete"

"$ROOT_DIR/build/more" --color=always -N "$WORK_DIR/pager.txt" > "$WORK_DIR/more_color.out"
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
    "$ROOT_DIR/build/make"
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
    "$ROOT_DIR/build/make"
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
    "$ROOT_DIR/build/make"
)
assert_file_contains "$WORK_DIR/make_gnuish/out.txt" '^shell-ok x-alpha x-beta$' "make did not handle repository-style functions and conditionals"

"$ROOT_DIR/build/netcat" -l 24681 > "$WORK_DIR/netcat_server.out" &
netcat_pid=$!
"$ROOT_DIR/build/sleep" 1
printf 'hello nc\n' | "$ROOT_DIR/build/netcat" localhost 24681 > "$WORK_DIR/netcat_client.out"
wait "$netcat_pid"
assert_file_contains "$WORK_DIR/netcat_server.out" 'hello nc' "netcat listener did not receive the TCP payload"

"$ROOT_DIR/build/netcat" -u -l -w 3 24682 > "$WORK_DIR/netcat_udp_server.out" &
netcat_udp_pid=$!
"$ROOT_DIR/build/sleep" 1
printf 'hello udp\n' | "$ROOT_DIR/build/netcat" -u -w 1 localhost 24682 > "$WORK_DIR/netcat_udp_client.out"
wait "$netcat_udp_pid"
assert_file_contains "$WORK_DIR/netcat_udp_server.out" 'hello udp' "netcat UDP mode did not receive the payload"

"$ROOT_DIR/build/netcat" -4 -k -l -s 127.0.0.1 -w 1500ms 24683 > "$WORK_DIR/netcat_keep_server.out" &
netcat_keep_pid=$!
"$ROOT_DIR/build/sleep" 1
printf 'hello keep one\n' | "$ROOT_DIR/build/netcat" -4 -n 127.0.0.1 24683 > "$WORK_DIR/netcat_keep_client1.out"
printf 'hello keep two\n' | "$ROOT_DIR/build/netcat" -4 -n 127.0.0.1 24683 > "$WORK_DIR/netcat_keep_client2.out"
wait "$netcat_keep_pid"
assert_file_contains "$WORK_DIR/netcat_keep_server.out" 'hello keep one' "netcat -k did not keep the listener alive for the first payload"
assert_file_contains "$WORK_DIR/netcat_keep_server.out" 'hello keep two' "netcat -k did not keep the listener alive for the second payload"

"$ROOT_DIR/build/shutdown" --help > "$WORK_DIR/shutdown_help.out" 2>&1
assert_file_contains "$WORK_DIR/shutdown_help.out" 'shutdown' "shutdown help output missing"

mkdir -p "$WORK_DIR/http_root"
printf 'hello from httpd\n' > "$WORK_DIR/http_root/index.txt"
printf 'hidden\n' > "$WORK_DIR/http_root/.secret"
printf 'outside root\n' > "$WORK_DIR/outside_root.txt"
ln "$WORK_DIR/outside_root.txt" "$WORK_DIR/http_root/hardlink.txt"
"$ROOT_DIR/build/httpd" -p "$HTTP_PORT_STATIC" -r "$WORK_DIR/http_root" > "$WORK_DIR/httpd.log" 2>&1 &
httpd_pid=$!
trap 'kill "$httpd_pid" 2>/dev/null || true' EXIT INT TERM
"$ROOT_DIR/build/sleep" 1
"$ROOT_DIR/build/wget" -q -O "$WORK_DIR/http_fetch.txt" "http://127.0.0.1:$HTTP_PORT_STATIC/index.txt"
assert_file_contains "$WORK_DIR/http_fetch.txt" '^hello from httpd$' "httpd did not serve the requested static file"
http_hidden_status=0
"$ROOT_DIR/build/wget" -q -O "$WORK_DIR/http_hidden.txt" "http://127.0.0.1:$HTTP_PORT_STATIC/.secret" > "$WORK_DIR/http_hidden.out" 2>&1 || http_hidden_status=$?
if [ "$http_hidden_status" -eq 0 ]; then
    fail "httpd should refuse to serve dotfiles from the document root"
fi
http_link_status=0
"$ROOT_DIR/build/wget" -q -O "$WORK_DIR/http_hardlink.txt" "http://127.0.0.1:$HTTP_PORT_STATIC/hardlink.txt" > "$WORK_DIR/http_hardlink.out" 2>&1 || http_link_status=$?
if [ "$http_link_status" -eq 0 ]; then
    fail "httpd should refuse multiply linked files from the document root"
fi
printf 'GET /index.txt\r\n\r\n' | "$ROOT_DIR/build/netcat" -4 -n -w 1 127.0.0.1 "$HTTP_PORT_STATIC" > "$WORK_DIR/http_bad_request.out"
assert_file_contains "$WORK_DIR/http_bad_request.out" '^HTTP/1\.1 400 Bad Request' "httpd should reject malformed requests without an HTTP version"
printf 'GET /index.txt HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\nTEST' | "$ROOT_DIR/build/netcat" -4 -n -w 1 127.0.0.1 "$HTTP_PORT_STATIC" > "$WORK_DIR/http_body_header.out"
assert_file_contains "$WORK_DIR/http_body_header.out" '^HTTP/1\.1 400 Bad Request' "httpd should reject GET requests with a request body framing header"
mkfifo "$WORK_DIR/http_idle_pipe"
"$ROOT_DIR/build/netcat" -4 -n 127.0.0.1 "$HTTP_PORT_STATIC" < "$WORK_DIR/http_idle_pipe" > "$WORK_DIR/http_idle_hold.out" 2>&1 &
httpd_idle_pid=$!
exec 9> "$WORK_DIR/http_idle_pipe"
"$ROOT_DIR/build/sleep" 1
http_idle_status=0
"$ROOT_DIR/build/timeout" 3s "$ROOT_DIR/build/wget" -q -O "$WORK_DIR/http_idle_fetch.txt" "http://127.0.0.1:$HTTP_PORT_STATIC/index.txt" > "$WORK_DIR/http_idle_fetch.out" 2>&1 || http_idle_status=$?
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
"$ROOT_DIR/build/timeout" 1s "$ROOT_DIR/build/httpd" -p "$HTTP_PORT_SERVICE" -c "$WORK_DIR/httpd_cli_conflict.conf" > "$WORK_DIR/httpd_cli_conflict.out" 2>&1 || http_conflict_rc=$?
if [ "$http_conflict_rc" -eq 0 ] || [ "$http_conflict_rc" -eq 124 ]; then
    fail "httpd should refuse conflicting CLI and config values rather than silently choosing one"
fi

cat > "$WORK_DIR/httpd.conf" <<EOF
command=$ROOT_DIR/build/httpd -p $HTTP_PORT_SERVICE -r $WORK_DIR/http_root
pidfile=$WORK_DIR/httpd.pid
stdout=$WORK_DIR/service-httpd.out
stderr=$WORK_DIR/service-httpd.err
EOF
"$ROOT_DIR/build/service" start "$WORK_DIR/httpd.conf" > "$WORK_DIR/service_start.out"
"$ROOT_DIR/build/sleep" 1
"$ROOT_DIR/build/service" status "$WORK_DIR/httpd.conf" > "$WORK_DIR/service_status.out"
assert_file_contains "$WORK_DIR/service_status.out" 'running' "service status did not report the daemon as running"
"$ROOT_DIR/build/wget" -q -O "$WORK_DIR/http_service_fetch.txt" "http://127.0.0.1:$HTTP_PORT_SERVICE/index.txt"
assert_file_contains "$WORK_DIR/http_service_fetch.txt" '^hello from httpd$' "service-managed httpd did not serve the requested static file"
cat > "$WORK_DIR/httpd_conflict.conf" <<EOF
command=$ROOT_DIR/build/httpd -p $HTTP_PORT_SERVICE -r $WORK_DIR/http_root
pidfile=$WORK_DIR/httpd-conflict.pid
stdout=$WORK_DIR/service-httpd-conflict.out
stderr=$WORK_DIR/service-httpd-conflict.err
EOF
service_conflict_status=0
"$ROOT_DIR/build/service" start "$WORK_DIR/httpd_conflict.conf" > "$WORK_DIR/service_conflict_start.out" 2>&1 || service_conflict_status=$?
if [ "$service_conflict_status" -eq 0 ]; then
    fail "service should report a startup failure when the requested port is already in use"
fi
"$ROOT_DIR/build/service" stop "$WORK_DIR/httpd.conf" > "$WORK_DIR/service_stop.out"
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
"$ROOT_DIR/build/httpd" -c "$WORK_DIR/httpd_direct_drop.conf" > "$WORK_DIR/httpd_direct_drop.log" 2>&1 &
httpd_drop_pid=$!
trap 'kill "$httpd_drop_pid" 2>/dev/null || true' EXIT INT TERM
"$ROOT_DIR/build/sleep" 1
"$ROOT_DIR/build/wget" -q -O "$WORK_DIR/http_direct_drop_fetch.txt" "http://127.0.0.1:$HTTP_PORT_DIRECT_DROP/index.txt"
assert_file_contains "$WORK_DIR/http_direct_drop_fetch.txt" '^hello from httpd$' "httpd with configured post-bind privilege drop did not serve the requested static file"
kill "$httpd_drop_pid" 2>/dev/null || true
wait "$httpd_drop_pid" 2>/dev/null || true
trap - EXIT INT TERM

cat > "$WORK_DIR/httpd_drop.conf" <<EOF
command=$ROOT_DIR/build/httpd -p $HTTP_PORT_SERVICE_DROP -r $WORK_DIR/http_root
pidfile=$WORK_DIR/httpd-drop.pid
stdout=$WORK_DIR/service-httpd-drop.out
stderr=$WORK_DIR/service-httpd-drop.err
user=$service_user
group=$service_group
EOF
"$ROOT_DIR/build/service" start "$WORK_DIR/httpd_drop.conf" > "$WORK_DIR/service_drop_start.out"
"$ROOT_DIR/build/sleep" 1
"$ROOT_DIR/build/wget" -q -O "$WORK_DIR/http_drop_fetch.txt" "http://127.0.0.1:$HTTP_PORT_SERVICE_DROP/index.txt"
assert_file_contains "$WORK_DIR/http_drop_fetch.txt" '^hello from httpd$' "service-managed httpd with configured privilege drop did not serve the requested static file"
"$ROOT_DIR/build/service" stop "$WORK_DIR/httpd_drop.conf" > "$WORK_DIR/service_drop_stop.out"

cat > "$WORK_DIR/httpd_bad_identity.conf" <<EOF
command=$ROOT_DIR/build/httpd -p $HTTP_PORT_BAD_ID -r $WORK_DIR/http_root
pidfile=$WORK_DIR/httpd-bad-identity.pid
stdout=$WORK_DIR/service-httpd-bad-identity.out
stderr=$WORK_DIR/service-httpd-bad-identity.err
user=this-user-should-not-exist-newos
EOF
bad_identity_status=0
"$ROOT_DIR/build/service" start "$WORK_DIR/httpd_bad_identity.conf" > "$WORK_DIR/service_bad_identity.out" 2>&1 || bad_identity_status=$?
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
"$ROOT_DIR/build/service" start "$WORK_DIR/quoted_service.conf" > "$WORK_DIR/quoted_service_start.out"
"$ROOT_DIR/build/sleep" 1
assert_file_contains "$WORK_DIR/service_quote/quoted.out" "^it's ok$" "service command parsing should preserve standard single-quote shell escaping"
"$ROOT_DIR/build/service" stop "$WORK_DIR/quoted_service.conf" > "$WORK_DIR/quoted_service_stop.out"

ln -sf "$WORK_DIR/real_service.log" "$WORK_DIR/symlink_service.log"
cat > "$WORK_DIR/symlink_log_service.conf" <<EOF
command=$ROOT_DIR/build/httpd -p $HTTP_PORT_LOG_SYMLINK -r $WORK_DIR/http_root
pidfile=$WORK_DIR/symlink-log.pid
stdout=$WORK_DIR/symlink_service.log
stderr=$WORK_DIR/symlink_service.log
EOF
symlink_log_status=0
"$ROOT_DIR/build/service" start "$WORK_DIR/symlink_log_service.conf" > "$WORK_DIR/symlink_log_service.out" 2>&1 || symlink_log_status=$?
if [ "$symlink_log_status" -eq 0 ]; then
    "$ROOT_DIR/build/service" stop "$WORK_DIR/symlink_log_service.conf" > /dev/null 2>&1 || true
    fail "service should reject symlinked log output targets"
fi

"$ROOT_DIR/build/sleep" 30 &
stray_pid=$!
printf '%s\n' "$stray_pid" > "$WORK_DIR/stray.pid"
cat > "$WORK_DIR/stray_service.conf" <<EOF
command=$ROOT_DIR/build/httpd -p $HTTP_PORT_STALE_PIDFILE -r $WORK_DIR/http_root
pidfile=$WORK_DIR/stray.pid
stdout=$WORK_DIR/stray.log
stderr=$WORK_DIR/stray.log
EOF
stray_stop_status=0
"$ROOT_DIR/build/service" stop "$WORK_DIR/stray_service.conf" > "$WORK_DIR/stray_stop.out" 2>&1 || stray_stop_status=$?
if ! kill -0 "$stray_pid" 2>/dev/null; then
    fail "service should not kill an unrelated process referenced only by a stale pidfile"
fi
kill "$stray_pid" 2>/dev/null || true
wait "$stray_pid" 2>/dev/null || true

unsafe_status=0
PATH="$WORK_DIR/bin:$PATH" "$ROOT_DIR/build/service" start "$WORK_DIR/unsafe_service.conf" > "$WORK_DIR/unsafe_service.out" 2>&1 || unsafe_status=$?
if [ "$unsafe_status" -eq 0 ]; then
    fail "service should reject PATH-only command execution for security reasons"
fi
