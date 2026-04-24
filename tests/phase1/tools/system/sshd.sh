#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup sshd

assert_command_succeeds "$ROOT_DIR/build/sshd" --help > "$WORK_DIR/sshd_help.out"
assert_file_contains "$WORK_DIR/sshd_help.out" '^sshd - minimal newos-native SSH server$' "sshd --help did not print the tool summary"
assert_file_contains "$WORK_DIR/sshd_help.out" '^Usage: sshd ' "sshd --help did not print usage"

sshd_status=0
"$ROOT_DIR/build/sshd" -p 0 -P secret -1 > "$WORK_DIR/sshd_bad_port.out" 2>&1 || sshd_status=$?
assert_exit_code "$sshd_status" '1' "sshd should reject an invalid port number"
assert_file_contains "$WORK_DIR/sshd_bad_port.out" '^Usage: sshd ' "sshd did not print usage for an invalid port"

sshd_user_status=0
"$ROOT_DIR/build/sshd" -u 'bad user' -P secret -1 > "$WORK_DIR/sshd_bad_user.out" 2>&1 || sshd_user_status=$?
assert_exit_code "$sshd_user_status" '1' "sshd should reject unsafe user names"
assert_file_contains "$WORK_DIR/sshd_bad_user.out" 'invalid user' "sshd did not reject an unsafe user name"

sshd_missing_password=0
"$ROOT_DIR/build/sshd" -1 > "$WORK_DIR/sshd_missing_password.out" 2>&1 || sshd_missing_password=$?
assert_exit_code "$sshd_missing_password" '1' "sshd should require a configured password"
assert_file_contains "$WORK_DIR/sshd_missing_password.out" '^Usage: sshd ' "sshd did not print usage when password is missing"

if command -v ssh >/dev/null 2>&1 && command -v timeout >/dev/null 2>&1 && { command -v sshpass >/dev/null 2>&1 || command -v setsid >/dev/null 2>&1; }; then
    port=$((25000 + ($$ % 1000)))
    printf '%s\n' '000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f' > "$WORK_DIR/host.seed"
    chmod 600 "$WORK_DIR/host.seed"
    "$ROOT_DIR/build/sshd" -1 -l 127.0.0.1 -p "$port" -u demo -P secret -k "$WORK_DIR/host.seed" > "$WORK_DIR/sshd_smoke_server.out" 2>&1 &
    sshd_pid=$!
    sleep 1
    ssh_smoke_status=0
    if command -v sshpass >/dev/null 2>&1; then
        timeout 20 sshpass -p secret ssh \
            -o StrictHostKeyChecking=no \
            -o UserKnownHostsFile="$WORK_DIR/known_hosts" \
            -o PreferredAuthentications=password \
            -o PubkeyAuthentication=no \
            -o NumberOfPasswordPrompts=1 \
            -o ConnectTimeout=5 \
            -o KexAlgorithms=curve25519-sha256 \
            -o HostKeyAlgorithms=ssh-ed25519 \
            -o Ciphers=chacha20-poly1305@openssh.com \
            -p "$port" demo@127.0.0.1 'printf sshd-smoke' > "$WORK_DIR/sshd_smoke_client.out" 2>&1 || ssh_smoke_status=$?
    else
        printf '#!/bin/sh\nprintf "%%s\\n" secret\n' > "$WORK_DIR/askpass.sh"
        chmod +x "$WORK_DIR/askpass.sh"
        timeout 20 env SSH_ASKPASS="$WORK_DIR/askpass.sh" SSH_ASKPASS_REQUIRE=force DISPLAY=newos setsid ssh \
            -o StrictHostKeyChecking=no \
            -o UserKnownHostsFile="$WORK_DIR/known_hosts" \
            -o PreferredAuthentications=password \
            -o PubkeyAuthentication=no \
            -o NumberOfPasswordPrompts=1 \
            -o ConnectTimeout=5 \
            -o KexAlgorithms=curve25519-sha256 \
            -o HostKeyAlgorithms=ssh-ed25519 \
            -o Ciphers=chacha20-poly1305@openssh.com \
            -p "$port" demo@127.0.0.1 'printf sshd-smoke' > "$WORK_DIR/sshd_smoke_client.out" 2>&1 || ssh_smoke_status=$?
    fi
    if [ "$ssh_smoke_status" -ne 0 ]; then
        kill "$sshd_pid" 2>/dev/null || true
    fi
    wait "$sshd_pid" || true
    assert_exit_code "$ssh_smoke_status" '0' "OpenSSH password exec smoke against sshd failed"
    assert_file_contains "$WORK_DIR/sshd_smoke_client.out" 'sshd-smoke' "sshd exec smoke did not return command output"
else
    note "skipping sshd OpenSSH smoke; ssh, timeout, and sshpass or setsid are required"
fi
