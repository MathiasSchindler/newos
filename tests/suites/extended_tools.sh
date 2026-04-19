#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"

WORK_DIR="$ROOT_DIR/tests/tmp/extended_tools"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

note "extended workflows"

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
    cat patch.diff | "$ROOT_DIR/build/patch" -p1
    "$ROOT_DIR/build/patch" -R -p1 -i patch.diff
)
assert_file_contains "$WORK_DIR/patch_target.txt" '^beta$' "patch reverse apply failed"
(
    cd "$WORK_DIR"
    "$ROOT_DIR/build/patch" -p1 -o patch_preview.txt -i patch.diff
)
assert_file_contains "$WORK_DIR/patch_preview.txt" '^gamma$' "patch -o did not write the patched preview output"

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

"$ROOT_DIR/build/shutdown" --help > "$WORK_DIR/shutdown_help.out" 2>&1
assert_file_contains "$WORK_DIR/shutdown_help.out" 'shutdown' "shutdown help output missing"
