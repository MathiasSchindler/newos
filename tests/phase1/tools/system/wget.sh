#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup wget

printf 'wget sample\n' > "$WORK_DIR/source.txt"
assert_command_succeeds "$ROOT_DIR/build/wget" -q -O "$WORK_DIR/copy.txt" "file://$WORK_DIR/source.txt"
assert_files_equal "$WORK_DIR/source.txt" "$WORK_DIR/copy.txt" "wget file:// download failed"

wget_stdout=$("$ROOT_DIR/build/wget" -q -O - "file://$WORK_DIR/source.txt" | tr -d '\r\n')
assert_text_equals "$wget_stdout" 'wget sample' "wget -O - did not stream the fetched content"

redirect_pid=
redirect_port_file="$WORK_DIR/redirect_port.txt"
printf 'redirect secret\n' > "$WORK_DIR/redirect_secret.txt"
cat > "$WORK_DIR/redirect_server.c" <<'EOF'
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int listen_fd;
    int client_fd;
    int opt = 1;
    struct sockaddr_in addr;
    socklen_t addr_len;
    FILE *port_file;
    char response[2048];

    if (argc != 3) {
        return 2;
    }
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return 3;
    }
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        close(listen_fd);
        return 4;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(listen_fd);
        return 5;
    }
    if (listen(listen_fd, 1) != 0) {
        close(listen_fd);
        return 6;
    }
    addr_len = sizeof(addr);
    if (getsockname(listen_fd, (struct sockaddr *)&addr, &addr_len) != 0) {
        close(listen_fd);
        return 8;
    }
    port_file = fopen(argv[1], "w");
    if (port_file == NULL) {
        close(listen_fd);
        return 9;
    }
    fprintf(port_file, "%u\n", (unsigned int)ntohs(addr.sin_port));
    fclose(port_file);
    client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) {
        close(listen_fd);
        return 7;
    }
    snprintf(response,
             sizeof(response),
             "HTTP/1.1 302 Found\r\nLocation: file://%s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
             argv[2]);
    (void)write(client_fd, response, strlen(response));
    close(client_fd);
    close(listen_fd);
    return 0;
}
EOF
"${CC:-cc}" -O2 "$WORK_DIR/redirect_server.c" -o "$WORK_DIR/redirect_server"

cleanup_redirect_server() {
    if [ -n "${redirect_pid:-}" ]; then
        kill "$redirect_pid" 2>/dev/null || true
        wait "$redirect_pid" 2>/dev/null || true
        redirect_pid=
    fi
}

trap 'cleanup_redirect_server' EXIT HUP INT TERM
"$WORK_DIR/redirect_server" "$redirect_port_file" "$WORK_DIR/redirect_secret.txt" > "$WORK_DIR/redirect_server.out" 2>&1 &
redirect_pid=$!
redirect_waits=0
while [ ! -s "$redirect_port_file" ] && [ "$redirect_waits" -lt 5 ]; do
    "$ROOT_DIR/build/sleep" 1
    redirect_waits=$((redirect_waits + 1))
done
[ -s "$redirect_port_file" ] || fail "redirect test server did not publish a port"
redirect_port=$(cat "$redirect_port_file" | tr -d ' \r\n')
redirect_status=0
"$ROOT_DIR/build/wget" -q -T 2s -O "$WORK_DIR/redirect_copy.txt" "http://127.0.0.1:$redirect_port/redirect" > "$WORK_DIR/redirect_fetch.out" 2>&1 || redirect_status=$?
cleanup_redirect_server
trap - EXIT HUP INT TERM
if [ "$redirect_status" -eq 0 ]; then
    fail "wget should reject redirects from HTTP to file:// URLs"
fi
if [ -s "$WORK_DIR/redirect_copy.txt" ]; then
    fail "wget should not copy local file contents after a redirect"
fi
assert_file_contains "$WORK_DIR/redirect_fetch.out" 'cannot follow redirect' "wget did not report the rejected redirect"
