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
        tty_cmd="\"$ROOT_DIR/build/sh\" -i"
        script -qfec "$tty_cmd" "$output_path" < "$input_path" > /dev/null 2>&1
    else
        tty_cmd="cat \"$input_path\" | \"$ROOT_DIR/build/sh\" -i"
        script -q /dev/null /bin/sh -c "$tty_cmd" > "$output_path" 2>&1
    fi
}

printf 'export FOO=bar\necho $FOO\nfalse\necho $?\necho ${FOO}\n' | "$ROOT_DIR/build/sh" > "$WORK_DIR/sh.out"
assert_file_contains "$WORK_DIR/sh.out" '^bar$' "shell variable expansion failed"
assert_file_contains "$WORK_DIR/sh.out" '^1$' "shell status expansion failed"

invalid_export_status=0
printf 'export BAD-NAME=value\n' | "$ROOT_DIR/build/sh" > "$WORK_DIR/export_invalid.out" 2>&1 || invalid_export_status=$?
[ "$invalid_export_status" -eq 1 ] || fail "shell export accepted an invalid environment name"
assert_file_contains "$WORK_DIR/export_invalid.out" '^sh: export: invalid name: BAD-NAME$' "shell export did not reject an invalid environment name"

printf 'echo first\necho second\nhistory\n' | "$ROOT_DIR/build/sh" > "$WORK_DIR/history.out"
assert_file_contains "$WORK_DIR/history.out" '1  echo first' "shell history missing first command"
assert_file_contains "$WORK_DIR/history.out" '2  echo second' "shell history missing second command"

long_pipeline='echo pipeline-ok'
i=1
while [ "$i" -le 12 ]; do
    long_pipeline="$long_pipeline | cat"
    i=$((i + 1))
done
printf '%s\n' "$long_pipeline" | "$ROOT_DIR/build/sh" > "$WORK_DIR/long_pipeline.out"
assert_file_contains "$WORK_DIR/long_pipeline.out" '^pipeline-ok$' "shell rejected a long pipeline"

many_args='echo'
i=1
while [ "$i" -le 80 ]; do
    many_args="$many_args arg$i"
    i=$((i + 1))
done
printf '%s\n' "$many_args" | "$ROOT_DIR/build/sh" > "$WORK_DIR/many_args.out"
assert_file_contains "$WORK_DIR/many_args.out" 'arg80$' "shell rejected a command with many arguments"

{
    i=1
    while [ "$i" -le 70 ]; do
        printf 'echo hist%d\n' "$i"
        i=$((i + 1))
    done
    printf 'history\n'
} | "$ROOT_DIR/build/sh" > "$WORK_DIR/long_history.out"
assert_file_contains "$WORK_DIR/long_history.out" '70  echo hist70' "shell history still capped below 70 entries"
assert_file_contains "$WORK_DIR/long_history.out" '71  history' "shell history did not retain the history command after many entries"

{
    i=1
    while [ "$i" -le 20 ]; do
        printf 'true &\n'
        i=$((i + 1))
    done
    printf 'jobs\n'
} | "$ROOT_DIR/build/sh" > "$WORK_DIR/many_jobs.out"
assert_file_contains "$WORK_DIR/many_jobs.out" '^\[20\] running true$' "shell jobs still capped below 20 entries"

{
    i=1
    while [ "$i" -le 40 ]; do
        printf 'alias a%d="echo alias-%d"\n' "$i" "$i"
        i=$((i + 1))
    done
    printf 'a40\n'
    i=1
    while [ "$i" -le 40 ]; do
        printf 'f%d() { echo function-%d; }\n' "$i" "$i"
        i=$((i + 1))
    done
    printf 'f40\n'
} | "$ROOT_DIR/build/sh" > "$WORK_DIR/many_aliases_functions.out"
assert_file_contains "$WORK_DIR/many_aliases_functions.out" '^alias-40$' "shell aliases still capped below 40 entries"
assert_file_contains "$WORK_DIR/many_aliases_functions.out" '^function-40$' "shell functions still capped below 40 entries"

printf 'echo interactive-flag-ok\n' | "$ROOT_DIR/build/sh" -i > "$WORK_DIR/interactive_flag.out"
assert_file_contains "$WORK_DIR/interactive_flag.out" '^interactive-flag-ok$' "shell -i flag handling failed"

printf 'echo one\\\n two\\\n three\n' | "$ROOT_DIR/build/sh" > "$WORK_DIR/continuation.out"
assert_file_contains "$WORK_DIR/continuation.out" '^one two three$' "shell line continuation failed"

printf 'echo tty-one\\\n tty-two\nexit\n' | "$ROOT_DIR/build/sh" -i > "$WORK_DIR/interactive_continuation.out"
assert_file_contains "$WORK_DIR/interactive_continuation.out" '^tty-one tty-two$' "shell interactive line continuation failed"

printf 'command -v ls\nalias hi="echo alias-ok"\nhi\nfn() { echo func-ok; }\nfn\ncat <<EOF\nheredoc-line\nEOF\n' | "$ROOT_DIR/build/sh" > "$WORK_DIR/features.out"
assert_file_contains "$WORK_DIR/features.out" '/ls$' "shell command -v failed"
assert_file_contains "$WORK_DIR/features.out" '^alias-ok$' "shell alias failed"
assert_file_contains "$WORK_DIR/features.out" '^func-ok$' "shell function failed"
assert_file_contains "$WORK_DIR/features.out" '^heredoc-line$' "shell here-document failed"

printf 'alias say="echo two words"\nsay\nshow() { echo "$1"; }\nshow "quoted text"\n' | "$ROOT_DIR/build/sh" > "$WORK_DIR/stability.out"
assert_file_contains "$WORK_DIR/stability.out" '^two words$' "shell quoted alias expansion failed"
assert_file_contains "$WORK_DIR/stability.out" '^quoted text$' "shell quoted function argument failed"

cat > "$WORK_DIR/args.sh" <<'EOF'
echo "0:$0"
echo "1:$1"
echo "2:$2"
echo "count:$#"
echo "all:$*"
set -- reset "two words" done
echo "set:$1|$2|$3|$#"
shift 2
echo "shift:$1|$#"
EOF
"$ROOT_DIR/build/sh" "$WORK_DIR/args.sh" alpha "beta gamma" > "$WORK_DIR/args.out"
assert_file_contains "$WORK_DIR/args.out" '^0:.*args\.sh$' "shell script name was not visible through \$0"
assert_file_contains "$WORK_DIR/args.out" '^1:alpha$' "shell \$1 expansion failed for script arguments"
assert_file_contains "$WORK_DIR/args.out" '^2:beta gamma$' "shell quoted script argument visibility failed"
assert_file_contains "$WORK_DIR/args.out" '^count:2$' "shell \$# expansion failed"
assert_file_contains "$WORK_DIR/args.out" '^all:alpha beta gamma$' "shell \$\\* expansion failed"
assert_file_contains "$WORK_DIR/args.out" '^set:reset|two words|done|3$' "shell set -- builtin failed"
assert_file_contains "$WORK_DIR/args.out" '^shift:done|1$' "shell shift builtin failed"

"$ROOT_DIR/build/sh" -c 'echo "$0|$1|$#"' invoked-name extra > "$WORK_DIR/cmode_args.out"
assert_file_contains "$WORK_DIR/cmode_args.out" '^invoked-name|extra|1$' "shell -c argument handling failed"

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

printf 'echo pasted-one\\\n pasted-two\nexit\n' > "$WORK_DIR/tty_continuation.input"
run_shell_tty "$WORK_DIR/tty_continuation.input" "$WORK_DIR/tty_continuation.out"
assert_file_contains "$WORK_DIR/tty_continuation.out" 'pasted-one pasted-two' "shell tty line continuation failed"
