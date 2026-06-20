#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    printf 'Usage: %s REPORT\n' "$0" >&2
    exit 1
fi

report=$1

awk -F, '
function is_data_row() {
    return $1 !~ /^#/ && $1 != "" && $1 != "case" && NF >= 10
}
function row_median_ns() {
    if (NF >= 15) return $9 + 0
    return $6 + 0
}
function row_speedup() {
    if (NF >= 15) return $14 + 0
    return $9 + 0
}
function row_active_workers() {
    if (NF >= 15) return $4 + 0
    return $3 + 0
}
function row_requested_width() {
    return $2 + 0
}
function remember_row(key) {
    median = row_median_ns()
    width = row_requested_width()
    active = row_active_workers()
    speed = row_speedup()
    if (median <= 0) return
    seen[key] = 1
    if (!(key in best_ns) || median < best_ns[key]) {
        best_ns[key] = median
        best_width[key] = width
        best_active[key] = active
        best_speed[key] = speed
    }
    row_count[key]++
    row_key[key, row_count[key]] = width SUBSEP active SUBSEP median SUBSEP speed
}
/^# section=/ {
    section = $0
    sub(/^# section=/, "", section)
    next
}
is_data_row() && section ~ /^width-sweep-/ {
    remember_row(section ":" $1)
}
END {
    print "section,case,best_requested_width,best_active_workers,best_median_ns,best_speedup,near_best_width,near_best_active_workers,near_best_median_ns,note"
    for (key in seen) {
        split(key, parts, ":")
        threshold = best_ns[key] * 1.05
        near_width = best_width[key]
        near_active = best_active[key]
        near_ns = best_ns[key]
        for (i = 1; i <= row_count[key]; ++i) {
            split(row_key[key, i], row, SUBSEP)
            width = row[1] + 0
            active = row[2] + 0
            median = row[3] + 0
            if (median <= threshold && width < near_width) {
                near_width = width
                near_active = active
                near_ns = median
            }
        }
        note = ""
        if (parts[2] == "memory" && near_width < best_width[key]) {
            note = "memory-like plateau; smaller width is near-best"
        } else if (parts[2] == "tasks" && best_width[key] <= 1) {
            note = "tiny task fan-out is serial-favored"
        } else if (parts[2] == "overhead") {
            note = "dispatch overhead floor; inspect absolute ns"
        } else if (near_width < best_width[key]) {
            note = "near-best at lower width"
        } else {
            note = "best width is also near-best width"
        }
        printf "%s,%s,%u,%u,%.0f,%.2f,%u,%u,%.0f,%s\n", parts[1], parts[2], best_width[key], best_active[key], best_ns[key], best_speed[key], near_width, near_active, near_ns, note
    }
}
' "$report"
