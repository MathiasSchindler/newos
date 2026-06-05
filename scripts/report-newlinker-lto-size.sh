#!/usr/bin/env bash
set -euo pipefail

LTO_WORK=${LTO_WORK:-build/freestanding-linux-newlinker-lto-report}
BASE_WORK=${BASE_WORK:-build/freestanding-linux-newlinker-nolto-report}
LINKER=${LINKER:-build/host-linux-x86_64/linker}
NEWLINKER_CC=${NEWLINKER_CC:-cc}
NEWLINKER_LINK_JOBS=${NEWLINKER_LINK_JOBS:-${PARALLEL_JOBS:-}}
TOP=${TOP:-20}

build_tree() {
  local work="$1" lto="$2"
  WORK="$work" \
    LINKER="$LINKER" \
    NEWLINKER_CC="$NEWLINKER_CC" \
    NEWLINKER_LTO="$lto" \
    NEWLINKER_LINK_JOBS="$NEWLINKER_LINK_JOBS" \
    bash build-freestanding-newlinker.sh >/dev/null
}

compare_sizes() {
  join -t $'\t' \
    <(sort -k1,1 "$BASE_WORK/successes.tsv") \
    <(sort -k1,1 "$LTO_WORK/successes.tsv")
}

rm -rf "$LTO_WORK" "$BASE_WORK"
build_tree "$BASE_WORK" 0
build_tree "$LTO_WORK" 1

printf 'NO_LTO_REPORT %s\n' "$BASE_WORK/report.txt"
grep -E 'RESULT_COUNTS|count=|size (true|false|cat|linker|ncc|ssh|wget)=' "$BASE_WORK/report.txt"
printf '\nLTO_REPORT %s\n' "$LTO_WORK/report.txt"
grep -E 'RESULT_COUNTS|LTO_MODE|count=|size (true|false|cat|linker|ncc|ssh|wget)=' "$LTO_WORK/report.txt"

printf '\nTOTAL_DELTA\n'
compare_sizes | awk -F '\t' '
  {
    off += $2; on += $3; delta = $3 - $2;
    if (delta < 0) wins++;
    else if (delta > 0) regressions++;
    else same++;
  }
  END {
    printf "tools=%d off=%d lto=%d delta=%+d wins=%d regressions=%d same=%d\n",
      wins + regressions + same, off, on, on - off, wins, regressions, same;
  }'

printf '\nTOP_REGRESSIONS\n'
compare_sizes | awk -F '\t' '{delta=$3-$2; if (delta > 0) printf "%d\t%s\t%d\t%d\n", delta, $1, $2, $3}' |
  sort -t $'\t' -k1,1nr | head -n "$TOP" |
  awk -F '\t' '{printf "%s\t%d\t%d\t%+d\n", $2, $3, $4, $1}'

printf '\nTOP_WINS\n'
compare_sizes | awk -F '\t' '{delta=$3-$2; if (delta < 0) printf "%d\t%s\t%d\t%d\n", delta, $1, $2, $3}' |
  sort -t $'\t' -k1,1n | head -n "$TOP" |
  awk -F '\t' '{printf "%s\t%d\t%d\t%+d\n", $2, $3, $4, $1}'
