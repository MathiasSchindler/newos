#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    printf 'Usage: %s BASELINE CURRENT\n' "$0" >&2
    exit 1
fi

base=$1
current=$2
tmp=${TMPDIR:-/tmp}/threading-benchcmp-$$.base
trap 'rm -f "$tmp"' EXIT HUP INT TERM

awk -F, '
function is_data_row() {
    return $1 !~ /^#/ && $1 != "" && $1 != "case" && NF >= 10
}
function key_for_row() {
    if (NF >= 15) return $1 "," $2 "," $3 "," $5 "," $6
    return $1 "," $2 "," $3 "," $4 "," $5
}
is_data_row() {
    if (NF >= 15) print key_for_row() "," ($4 + 0) "," ($7 + 0) "," ($9 + 0) "," ($14 + 0)
    else print key_for_row() "," 0 "," 0 "," ($6 + 0) "," ($9 + 0)
}
' "$base" > "$tmp"

awk -F, '
function is_data_row() {
    return $1 !~ /^#/ && $1 != "" && $1 != "case" && NF >= 10
}
function key_for_row() {
    if (NF >= 15) return $1 "," $2 "," $3 "," $5 "," $6
    return $1 "," $2 "," $3 "," $4 "," $5
}
function print_row(key, old_active, new_active, old_effective_chunk, new_effective_chunk, old_ns, new_ns, old_speed, new_speed) {
    ratio = old_ns > 0 ? new_ns / old_ns : 0
    change = old_ns > 0 ? ((new_ns - old_ns) * 100.0) / old_ns : 0
    speed_delta = new_speed - old_speed
    printf "%s,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.2f,%+.2f,%+.2f\n", key, old_active, new_active, old_effective_chunk, new_effective_chunk, old_ns, new_ns, ratio, change, speed_delta
}
NR == FNR {
    key = $1 "," $2 "," $3 "," $4 "," $5
    base_active[key] = $6 + 0
    base_effective_chunk[key] = $7 + 0
    base_ns[key] = $8 + 0
    base_speed[key] = $9 + 0
    seen_base[key] = 1
    next
}
is_data_row() {
    key = key_for_row()
    if (NF >= 15) {
        current_active[key] = $4 + 0
        current_effective_chunk[key] = $7 + 0
        current_ns[key] = $9 + 0
        current_speed[key] = $14 + 0
    } else {
        current_active[key] = 0
        current_effective_chunk[key] = 0
        current_ns[key] = $6 + 0
        current_speed[key] = $9 + 0
    }
    seen_current[key] = 1
    next
}
END {
    print "case,requested_width,effective_width,units,requested_min_chunk,baseline_active_workers,current_active_workers,baseline_effective_min_chunk,current_effective_min_chunk,baseline_median_ns,current_median_ns,time_ratio,time_change_pct,speedup_delta"
    for (key in seen_current) {
        if (seen_base[key]) {
            print_row(key, base_active[key], current_active[key], base_effective_chunk[key], current_effective_chunk[key], base_ns[key], current_ns[key], base_speed[key], current_speed[key])
        }
    }
    for (key in seen_base) {
        if (!seen_current[key]) {
            printf "# missing-current %s\n", key
        }
    }
    for (key in seen_current) {
        if (!seen_base[key]) {
            printf "# new-current %s\n", key
        }
    }
}
' "$tmp" "$current"
