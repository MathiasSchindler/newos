#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup strace

if [ "$(uname -s 2>/dev/null || echo unknown)" != Linux ] || [ "$(uname -m 2>/dev/null || echo unknown)" != x86_64 ]; then
    note "phase1 system: strace skipped on non-Linux-x86_64 host"
    exit 0
fi

printf 'alpha\nbeta\n' > "$WORK_DIR/input.txt"

assert_command_succeeds "${TEST_BIN_DIR}/strace" -e execve,openat,read,write "${TEST_BIN_DIR}/cat" "$WORK_DIR/input.txt" > "$WORK_DIR/cat.stdout" 2> "$WORK_DIR/cat.stderr"
assert_files_equal "$WORK_DIR/input.txt" "$WORK_DIR/cat.stdout" "Linux strace should preserve traced command stdout"
assert_file_contains "$WORK_DIR/cat.stderr" 'execve("' "Linux strace did not decode execve path arguments"
assert_file_contains "$WORK_DIR/cat.stderr" 'openat(0xffffffffffffff9c, ".*input.txt", O_RDONLY' "Linux strace did not decode openat path arguments"
assert_file_contains "$WORK_DIR/cat.stderr" 'read(0x' "Linux strace did not trace read calls"
assert_file_contains "$WORK_DIR/cat.stderr" 'write(0x' "Linux strace did not trace write calls"

assert_command_succeeds "${TEST_BIN_DIR}/strace" --json -p -T -e openat "${TEST_BIN_DIR}/cat" "$WORK_DIR/input.txt" > "$WORK_DIR/cat_json.stdout" 2> "$WORK_DIR/cat_json.stderr"
assert_files_equal "$WORK_DIR/input.txt" "$WORK_DIR/cat_json.stdout" "Linux strace --json should preserve traced command stdout"
assert_file_contains "$WORK_DIR/cat_json.stderr" '"event":"syscall"' "Linux strace --json did not emit syscall events"
assert_file_contains "$WORK_DIR/cat_json.stderr" '"pid":' "Linux strace --json did not include pid metadata"
assert_file_contains "$WORK_DIR/cat_json.stderr" '"decoded":{"arg":1,"kind":"string","value":".*input.txt","truncated":false}' "Linux strace --json did not include decoded openat paths"

assert_command_succeeds "${TEST_BIN_DIR}/strace" -o "$WORK_DIR/echo.trace" -e write "${TEST_BIN_DIR}/echo" hello > "$WORK_DIR/echo.stdout" 2> "$WORK_DIR/echo.stderr"
assert_file_contains "$WORK_DIR/echo.stdout" '^hello$' "Linux strace -o should preserve target stdout"
assert_file_contains "$WORK_DIR/echo.trace" '^write' "Linux strace -o did not write trace output file"
if [ -s "$WORK_DIR/echo.stderr" ]; then
    fail "Linux strace -o should not write trace lines to stderr"
fi

assert_command_succeeds "${TEST_BIN_DIR}/strace" -c "${TEST_BIN_DIR}/echo" hello > "$WORK_DIR/summary.stdout" 2> "$WORK_DIR/summary.stderr"
assert_file_contains "$WORK_DIR/summary.stdout" '^hello$' "Linux strace -c should preserve target stdout"
assert_file_contains "$WORK_DIR/summary.stderr" '^syscall calls errors bytes avg_bytes total_ms avg_ms max_ms$' "Linux strace -c did not print summary header"
assert_file_contains "$WORK_DIR/summary.stderr" '^write 1 0 6 ' "Linux strace -c did not summarize write byte counts"

if command -v "${CC:-cc}" >/dev/null 2>&1; then
    cat > "$WORK_DIR/strace_helpers.c" <<'EOF'
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0] == 'f') {
        pid_t pid = fork();
        if (pid == 0) {
            if (write(1, "child-strace-follow\n", 20) < 0) return 1;
            _exit(0);
        }
        if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
            return 0;
        }
        return 1;
    }
    if (argc > 1 && argv[1][0] == 's') {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        if (fd < 0) return 1;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(1);
        addr.sin_addr.s_addr = htonl(0x7f000001U);
        connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        close(fd);
        return 0;
    }
    if (argc > 1 && argv[1][0] == 'p') {
        int fds[2];
        struct pollfd pfd;
        if (pipe(fds) != 0) return 1;
        if (write(fds[1], "x", 1) < 0) return 1;
        pfd.fd = fds[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        poll(&pfd, 1, 0);
        close(fds[0]);
        close(fds[1]);
        return 0;
    }
    return 1;
}
EOF
    "${CC:-cc}" -O2 "$WORK_DIR/strace_helpers.c" -o "$WORK_DIR/strace_helpers"

    assert_command_succeeds "${TEST_BIN_DIR}/strace" -p -e write "$WORK_DIR/strace_helpers" fork > "$WORK_DIR/fork.stdout" 2> "$WORK_DIR/fork.stderr"
    assert_file_contains "$WORK_DIR/fork.stdout" '^child-strace-follow$' "Linux strace should preserve followed child stdout"
    assert_file_contains "$WORK_DIR/fork.stderr" '^\[[0-9][0-9]*] write(0x1,' "Linux strace did not follow forked child write syscalls"

    assert_command_succeeds "${TEST_BIN_DIR}/strace" -e connect "$WORK_DIR/strace_helpers" socket > "$WORK_DIR/socket.stdout" 2> "$WORK_DIR/socket.stderr"
    assert_file_contains "$WORK_DIR/socket.stderr" 'connect(0x[0-9a-f]*, 127\.0\.0\.1:1, 0x10)' "Linux strace did not decode IPv4 socket addresses"

    assert_command_succeeds "${TEST_BIN_DIR}/strace" -e poll "$WORK_DIR/strace_helpers" poll > "$WORK_DIR/poll.stdout" 2> "$WORK_DIR/poll.stderr"
    assert_file_contains "$WORK_DIR/poll.stderr" 'poll(\[{fd=[0-9][0-9]*,events=POLLIN}], 0x1, 0x0) = 1' "Linux strace did not decode pollfd arrays"
else
    note "phase1 system: strace helper coverage skipped; no C compiler found"
fi