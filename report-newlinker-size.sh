#!/usr/bin/env bash
set -euo pipefail

WORK=${WORK:-build/freestanding-linux-newlinker-report}
LINKER=${LINKER:-build/host-linux-x86_64/linker}
TOOLS=${TOOLS:-true false cat linker ncc ssh wget}
TOP=${TOP:-8}

if [[ ! -d "$WORK/.maps" || ! -s "$WORK/successes.tsv" ]]; then
  LINKER_REPORTS=1 WORK="$WORK" LINKER="$LINKER" bash build-freestanding-newlinker.sh >/dev/null
fi

printf 'tool\tfile\ttext\tdata\tbss\tmemory\tfolded\tdiscarded\n'
for tool in $TOOLS; do
  key=$tool
  map_name=$tool
  [[ "$tool" == "[" ]] && map_name=lbracket
  map=$WORK/.maps/$map_name.map
  log=$WORK/.logs/link-${tool//\//_}.log
  [[ "$tool" == "[" ]] && log=$WORK/.logs/link-lbracket.log
  if [[ ! -s "$map" ]]; then
    printf '%s\tmissing-map\n' "$tool"
    continue
  fi
  file_size=$(awk -F '\t' -v n="$key" '$1==n {print $2}' "$WORK/successes.tsv")
  read -r text_size data_size bss_size map_file_size memory_size < <(awk '/^text\/data\/bss\/file\/memory:/ {sub(/^text\/data\/bss\/file\/memory:[[:space:]]*/, ""); gsub("/", " "); print; exit}' "$map")
  read -r folded discarded < <(awk '/^folded\/discarded bytes:/ {sub(/^folded\/discarded bytes:[[:space:]]*/, ""); gsub("/", " "); print; found=1} END {if (!found) print "0 0"}' "$log")
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$tool" "${file_size:-$map_file_size}" "$text_size" "$data_size" "$bss_size" "$memory_size" "$folded" "$discarded"
done

for tool in $TOOLS; do
  map_name=$tool
  [[ "$tool" == "[" ]] && map_name=lbracket
  map=$WORK/.maps/$map_name.map
  [[ -s "$map" ]] || continue
  printf '\n# %s top live sections\n' "$tool"
  awk '
    /^Live sections:/ {live=1; next}
    live && NF >= 4 {
      size=$2; section=$3; object=$4;
      folded=index($0, " folded-to=") ? " folded" : "";
      print size "\t" section "\t" object folded;
    }
  ' "$map" | sort -nr | head -n "$TOP"
  printf '\n# %s top live objects\n' "$tool"
  awk '
    /^Live sections:/ {live=1; next}
    live && NF >= 4 {object[$4]+=$2}
    END {for (name in object) print object[name] "\t" name}
  ' "$map" | sort -nr | head -n "$TOP"
done
