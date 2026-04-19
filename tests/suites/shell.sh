#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"

WORK_DIR="$ROOT_DIR/tests/tmp/shell"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

note "shell"

run_shell_tty() {
    input_path=$1
    output_path=$2

    if ! command -v script >/dev/null 2>&1; then
        note "shell tty checks skipped: script(1) not available"
        return 0
    fi

    if script --version >/dev/null 2>&1; then
        script -qfec "$ROOT_DIR/build/sh -i" /dev/null < "$input_path" > "$output_path" 2>&1
    else
        script -q /dev/null "$ROOT_DIR/build/sh" -i < "$input_path" > "$output_path" 2>&1
    fi
}

printf 'export FOO=bar\necho $FOO\nfalse\necho $?\necho ${FOO}\n' | "$ROOT_DIR/build/sh" > "$WORK_DIR/sh.out"
assert_file_contains "$WORK_DIR/sh.out" '^bar$' "shell variable expansion failed"
assert_file_contains "$WORK_DIR/sh.out" '^1$' "shell status expansion failed"

printf 'echo first\necho second\nhistory\n' | "$ROOT_DIR/build/sh" > "$WORK_DIR/history.out"
assert_file_contains "$WORK_DIR/history.out" '1  echo first' "shell history missing first command"
assert_file_contains "$WORK_DIR/history.out" '2  echo second' "shell history missing second command"

printf 'echo interactive-flag-ok\n' | "$ROOT_DIR/build/sh" -i > "$WORK_DIR/interactive_flag.out"
assert_file_contains "$WORK_DIR/interactive_flag.out" '^interactive-flag-ok$' "shell -i flag handling failed"

printf 'command -v ls\nalias hi="echo alias-ok"\nhi\nfn() { echo func-ok; }\nfn\ncat <<EOF\nheredoc-line\nEOF\n' | "$ROOT_DIR/build/sh" > "$WORK_DIR/features.out"
assert_file_contains "$WORK_DIR/features.out" '/ls$' "shell command -v failed"
assert_file_contains "$WORK_DIR/features.out" '^alias-ok$' "shell alias failed"
assert_file_contains "$WORK_DIR/features.out" '^func-ok$' "shell function failed"
assert_file_contains "$WORK_DIR/features.out" '^heredoc-line$' "shell here-document failed"

printf 'alias say="echo two words"\nsay\nshow() { echo "$1"; }\nshow "quoted text"\n' | "$ROOT_DIR/build/sh" > "$WORK_DIR/stability.out"
assert_file_contains "$WORK_DIR/stability.out" '^two words$' "shell quoted alias expansion failed"
assert_file_contains "$WORK_DIR/stability.out" '^quoted text$' "shell quoted function argument failed"
printf 'echo unicode-space\n' | "$ROOT_DIR/build/sh" > "$WORK_DIR/unicode_space.out"
assert_file_contains "$WORK_DIR/unicode_space.out" '^unicode-space$' "shell did not treat Unicode whitespace as a separator"

before_docs=$(find /tmp -maxdepth 1 -name 'newos-sh-heredoc-*' 2>/dev/null | wc -l | tr -d ' ')
printf 'cat <<EOF\ncleanup-check\nEOF\n' | "$ROOT_DIR/build/sh" > "$WORK_DIR/heredoc_cleanup.out"
after_docs=$(find /tmp -maxdepth 1 -name 'newos-sh-heredoc-*' 2>/dev/null | wc -l | tr -d ' ')
assert_file_contains "$WORK_DIR/heredoc_cleanup.out" '^cleanup-check$' "shell heredoc cleanup run failed"
assert_text_equals "$after_docs" "$before_docs" "shell heredoc temp files leaked"

printf 'ec\t tab-complete-ok\nexit\n' > "$WORK_DIR/tty_completion.input"
run_shell_tty "$WORK_DIR/tty_completion.input" "$WORK_DIR/tty_completion.out"
assert_file_contains "$WORK_DIR/tty_completion.out" 'tab-complete-ok' "shell interactive tab completion failed"

printf 'world\001echo hello \nexit\n' > "$WORK_DIR/tty_ctrl_a.input"
run_shell_tty "$WORK_DIR/tty_ctrl_a.input" "$WORK_DIR/tty_ctrl_a.out"
assert_file_contains "$WORK_DIR/tty_ctrl_a.out" 'hello world' "shell interactive Ctrl-A line editing failed"
