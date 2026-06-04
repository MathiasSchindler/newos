#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR=${NEWOS_MACOS_NEWLINKER_BUILD_DIR:-${MACOS_FREESTANDING_BUILD_DIR:-build/newlinker-macos-aarch64}}
TOOLS=${TOOLS:-true false echo printf date ls cat sh readelf ncc ssh wget nm size expack}
COMPARE_FILE=

while [[ $# -gt 0 ]]; do
  case "$1" in
    --compare)
      if [[ $# -lt 2 ]]; then
        echo "missing path after --compare" >&2
        exit 1
      fi
      COMPARE_FILE=$2
      shift 2
      ;;
    --compare=*)
      COMPARE_FILE=${1#--compare=}
      shift
      ;;
    -h|--help)
      echo "usage: $0 [--compare previous.tsv]" >&2
      exit 0
      ;;
    *)
      echo "unknown option: $1" >&2
      exit 1
      ;;
  esac
done


if [[ ! -d "$BUILD_DIR" ]]; then
  echo "missing macOS freestanding build directory: $BUILD_DIR" >&2
  echo "build it first with make freestanding" >&2
  exit 1
fi
if ! command -v otool >/dev/null 2>&1; then
  echo "missing otool; cannot measure Mach-O section bytes" >&2
  exit 1
fi
if [[ -n "$COMPARE_FILE" && ! -f "$COMPARE_FILE" ]]; then
  echo "missing compare file: $COMPARE_FILE" >&2
  exit 1
fi

section_bytes() {
  local file=$1 total=0 key value hex in_section=0 section_size= section_offset=
  add_section() {
    if [[ -n "$section_size" && -n "$section_offset" && "$section_offset" != 0 ]]; then
      hex=${section_size#0x}
      total=$((total + 16#$hex))
    fi
  }
  while read -r key value _; do
    if [[ "$key" == "Section" ]]; then
      add_section
      in_section=1
      section_size=
      section_offset=
    elif [[ "$in_section" == 1 && "$key" == "size" && "$value" == 0x* ]]; then
      section_size=$value
    elif [[ "$in_section" == 1 && "$key" == "offset" ]]; then
      section_offset=$value
    fi
  done < <(otool -l "$file")
  add_section
  printf '%s' "$total"
}

load_command_summary() {
  local file=$1 ncmds= build_tools=unknown build_seen=0 key value
  ncmds=$(otool -h "$file" | awk 'NF >= 8 && $1 ~ /^0x/ { print $6; exit }')
  while read -r key value _; do
    if [[ "$key" == "cmd" && "$value" == "LC_BUILD_VERSION" ]]; then
      build_seen=1
    elif [[ "$build_seen" == 1 && "$key" == "ntools" ]]; then
      build_tools=$value
      build_seen=0
    fi
  done < <(otool -l "$file")
  printf '%s\t%s' "${ncmds:-unknown}" "$build_tools"
}

if [[ -n "$COMPARE_FILE" ]]; then
  printf 'tool\tfile_bytes\tfile_section_bytes\traster_delta\tload_commands\tbuild_tools\tdelta_file_bytes\tdelta_file_section_bytes\n'
else
  printf 'tool\tfile_bytes\tfile_section_bytes\traster_delta\tload_commands\tbuild_tools\n'
fi
for tool in $TOOLS; do
  file="$BUILD_DIR/$tool"
  if [[ ! -f "$file" ]]; then
    if [[ -n "$COMPARE_FILE" ]]; then
      printf '%s\tmissing\tmissing\tmissing\tmissing\tmissing\tmissing\tmissing\n' "$tool"
    else
      printf '%s\tmissing\tmissing\tmissing\tmissing\tmissing\n' "$tool"
    fi
    continue
  fi
  file_bytes=$(wc -c < "$file" | tr -d ' ')
  sections=$(section_bytes "$file")
  load_summary=$(load_command_summary "$file")
  if [[ -n "$COMPARE_FILE" ]]; then
    previous=$(awk -F '\t' -v tool="$tool" 'NR > 1 && $1 == tool { print $2 "\t" $3; exit }' "$COMPARE_FILE")
    if [[ -n "$previous" ]]; then
      previous_file=${previous%%$'\t'*}
      previous_sections=${previous#*$'\t'}
      if [[ "$previous_file" =~ ^[0-9]+$ && "$previous_sections" =~ ^[0-9]+$ ]]; then
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$tool" "$file_bytes" "$sections" "$((file_bytes - sections))" "$load_summary" "$((file_bytes - previous_file))" "$((sections - previous_sections))"
      else
        printf '%s\t%s\t%s\t%s\t%s\tmissing\tmissing\n' "$tool" "$file_bytes" "$sections" "$((file_bytes - sections))" "$load_summary"
      fi
    else
      printf '%s\t%s\t%s\t%s\t%s\tnew\tnew\n' "$tool" "$file_bytes" "$sections" "$((file_bytes - sections))" "$load_summary"
    fi
  else
    printf '%s\t%s\t%s\t%s\t%s\n' "$tool" "$file_bytes" "$sections" "$((file_bytes - sections))" "$load_summary"
  fi
done