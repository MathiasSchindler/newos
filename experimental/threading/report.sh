#!/bin/sh
set -eu

DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
cd "$DIR"

build=1
if [ "${1:-}" = "--no-build" ]; then
    build=0
    shift
fi

if [ "$build" -ne 0 ]; then
    make freestanding
fi

bench=./build/threadbench
max_width=${THREADBENCH_MAX_WIDTH:-16}
repeat=${THREADBENCH_REPEAT:-5}
cpu_items=${THREADBENCH_CPU_ITEMS:-1048576}
cpu_rounds=${THREADBENCH_CPU_ROUNDS:-128}
memory_items=${THREADBENCH_MEMORY_ITEMS:-1048576}
memory_rounds=${THREADBENCH_MEMORY_ROUNDS:-8}
task_count=${THREADBENCH_TASKS:-65536}
task_rounds=${THREADBENCH_TASK_ROUNDS:-16}
overhead_items=${THREADBENCH_OVERHEAD_ITEMS:-262144}

printf '# host=%s arch=%s\n' "$(uname -s 2>/dev/null || printf unknown)" "$(uname -m 2>/dev/null || printf unknown)"
if command -v sysctl >/dev/null 2>&1; then
    logical=$(sysctl -n hw.logicalcpu 2>/dev/null || printf unknown)
    physical=$(sysctl -n hw.physicalcpu 2>/dev/null || printf unknown)
    printf '# host_logical_cpus=%s host_physical_cpus=%s\n' "$logical" "$physical"
fi

printf '\n# section=width-sweep-cpu\n'
"$bench" --case mix --items "$cpu_items" --rounds "$cpu_rounds" --repeat "$repeat" --max-width "$max_width" --min-chunk 4096

printf '\n# section=width-sweep-memory\n'
"$bench" --case memory --items "$memory_items" --rounds "$memory_rounds" --repeat "$repeat" --max-width "$max_width" --min-chunk 4096

printf '\n# section=width-sweep-tasks\n'
"$bench" --case tasks --tasks "$task_count" --rounds "$task_rounds" --repeat "$repeat" --max-width "$max_width"

printf '\n# section=width-sweep-overhead\n'
"$bench" --case overhead --items "$overhead_items" --repeat "$repeat" --max-width "$max_width"

printf '\n# section=chunk-sweep-cpu\n'
for chunk in 1 8 64 512 4096 32768; do
    printf '# chunk=%s\n' "$chunk"
    "$bench" --case mix --items 262144 --rounds 16 --repeat 3 --max-width "$max_width" --min-chunk "$chunk"
done

printf '\n# section=size-sweep-cpu\n'
for items in 4096 65536 1048576; do
    printf '# items=%s\n' "$items"
    "$bench" --case mix --items "$items" --rounds 64 --repeat 3 --max-width "$max_width" --min-chunk 4096
done