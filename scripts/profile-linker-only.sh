#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

BUILD_DIR=${BUILD_DIR:-build/newlinker-macos-aarch64}
LINKER_BIN=${LINKER_BIN:-build/host-macos-aarch64/linker}
TOOLS=${TOOLS:-ncc linker ssh wget pgpmsg pdfgrep xmlquery}
REPEATS=${REPEATS:-15}
WARMUP=${WARMUP:-2}
WORK=${WORK:-$BUILD_DIR/.linker-only-profile}

if [[ ! -x "$LINKER_BIN" ]]; then
  echo "missing linker binary: $LINKER_BIN" >&2
  echo "build it first: make $LINKER_BIN" >&2
  exit 1
fi

if [[ ! -d "$BUILD_DIR/.obj" ]]; then
  echo "missing object directory: $BUILD_DIR/.obj" >&2
  echo "build freestanding objects first: make freestanding" >&2
  exit 1
fi

if ! [[ "$REPEATS" =~ ^[0-9]+$ ]] || (( REPEATS < 1 )); then
  echo "REPEATS must be >= 1" >&2
  exit 1
fi

if ! [[ "$WARMUP" =~ ^[0-9]+$ ]]; then
  echo "WARMUP must be >= 0" >&2
  exit 1
fi

mkdir -p "$WORK"
cmds_tsv="$WORK/commands.tsv"
results_tsv="$WORK/results.tsv"
skipped_tsv="$WORK/skipped.tsv"
: > "$cmds_tsv"
: > "$skipped_tsv"

for tool in $TOOLS; do
  target="$BUILD_DIR/$tool"
  rm -f "$target"
  line=$(make -n "$target" 2>/dev/null | awk '/\/linker --target=mach-o-arm64/ {print; found=1} END{if(!found) exit 1}') || true
  if [[ -z "$line" ]]; then
    printf '%s\t%s\n' "$tool" "not linked via build/newlinker linker command" >> "$skipped_tsv"
    continue
  fi
  printf '%s\t%s\n' "$tool" "$line" >> "$cmds_tsv"
done

if [[ ! -s "$cmds_tsv" ]]; then
  echo "no eligible tools found in TOOLS list" >&2
  [[ -s "$skipped_tsv" ]] && cat "$skipped_tsv" >&2
  exit 1
fi

python3 - "$cmds_tsv" "$results_tsv" "$WORK" "$REPEATS" "$WARMUP" <<'PY'
import csv
import os
import shlex
import statistics
import subprocess
import sys
import time

cmds_tsv, results_tsv, work, repeats_s, warmup_s = sys.argv[1:]
repeats = int(repeats_s)
warmup = int(warmup_s)

rows = []
with open(cmds_tsv, "r", newline="") as f:
    for tool, cmd in csv.reader(f, delimiter="\t"):
        rows.append((tool, cmd))

run_rows = []
summary_rows = []

for tool, cmd in rows:
    argv = shlex.split(cmd)
    if "-o" not in argv:
        raise RuntimeError(f"missing -o in command for {tool}")
    out_index = argv.index("-o") + 1
    if out_index >= len(argv):
        raise RuntimeError(f"invalid -o in command for {tool}")

    out_dir = os.path.join(work, "bins")
    os.makedirs(out_dir, exist_ok=True)

    timings = []
    for i in range(repeats):
        out_path = os.path.join(out_dir, f"{tool}.run{i}")
        cur_argv = list(argv)
        cur_argv[out_index] = out_path

        t0 = time.perf_counter()
        proc = subprocess.run(cur_argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        dt = time.perf_counter() - t0

        if proc.returncode != 0:
            raise RuntimeError(f"link command failed for {tool} run {i+1}")

        timings.append(dt)
        run_rows.append((tool, i + 1, dt))

    warm = timings[warmup:] if warmup < len(timings) else timings
    p95 = statistics.quantiles(warm, n=20)[18] if len(warm) >= 20 else max(warm)
    summary_rows.append((tool, len(timings), statistics.mean(warm), min(warm), p95, max(warm)))

with open(results_tsv, "w", newline="") as f:
    w = csv.writer(f, delimiter="\t")
    w.writerow(["tool", "runs", "warm_avg_s", "warm_min_s", "warm_p95_s", "warm_max_s"])
    for row in summary_rows:
        w.writerow([row[0], row[1], f"{row[2]:.6f}", f"{row[3]:.6f}", f"{row[4]:.6f}", f"{row[5]:.6f}"])

print("tool\truns\twarm_avg_s\twarm_min_s\twarm_p95_s\twarm_max_s")
for row in summary_rows:
    print(f"{row[0]}\t{row[1]}\t{row[2]:.6f}\t{row[3]:.6f}\t{row[4]:.6f}\t{row[5]:.6f}")
PY

echo
echo "results: $results_tsv"
echo "commands: $cmds_tsv"
if [[ -s "$skipped_tsv" ]]; then
  echo "skipped: $skipped_tsv"
fi