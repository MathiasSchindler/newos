#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR=${NEWOS_MACOS_NEWLINKER_BUILD_DIR:-${MACOS_FREESTANDING_BUILD_DIR:-build/newlinker-macos-aarch64}}
MAP_DIR=${NEWOS_MACOS_NEWLINKER_MAP_DIR:-}
TOOLS=${TOOLS:-true false echo printf date ls cat sh readelf ncc ssh wget nm size expack}
COMPARE_FILE=
TOP_COUNT=${NEWOS_SIZE_REPORT_TOP_COUNT:-3}

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
    --maps)
      if [[ $# -lt 2 ]]; then
        echo "missing path after --maps" >&2
        exit 1
      fi
      MAP_DIR=$2
      shift 2
      ;;
    --maps=*)
      MAP_DIR=${1#--maps=}
      shift
      ;;
    --top)
      if [[ $# -lt 2 ]]; then
        echo "missing count after --top" >&2
        exit 1
      fi
      TOP_COUNT=$2
      shift 2
      ;;
    --top=*)
      TOP_COUNT=${1#--top=}
      shift
      ;;
    -h|--help)
      echo "usage: $0 [--compare previous.tsv] [--maps DIR] [--top N]" >&2
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
if ! [[ "$TOP_COUNT" =~ ^[0-9]+$ ]] || [[ "$TOP_COUNT" -eq 0 ]]; then
  echo "invalid --top count: $TOP_COUNT" >&2
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

top_file_sections() {
  local file=$1 tmp=${TMPDIR:-/tmp}/newos-macho-sections.$$.tsv
  local key value in_section=0 section_name= section_segment= section_size= section_offset= hex
  : > "$tmp"
  add_section() {
    if [[ -n "$section_name" && -n "$section_segment" && -n "$section_size" && -n "$section_offset" && "$section_offset" != 0 ]]; then
      hex=${section_size#0x}
      printf '%s\t%s,%s\n' "$((16#$hex))" "$section_segment" "$section_name" >> "$tmp"
    fi
  }
  while read -r key value _; do
    if [[ "$key" == "Section" ]]; then
      add_section
      in_section=1
      section_name=
      section_segment=
      section_size=
      section_offset=
    elif [[ "$key" == "Load" ]]; then
      add_section
      in_section=0
      section_name=
      section_segment=
      section_size=
      section_offset=
    elif [[ "$in_section" == 1 && "$key" == "sectname" ]]; then
      section_name=$value
    elif [[ "$in_section" == 1 && "$key" == "segname" ]]; then
      section_segment=$value
    elif [[ "$in_section" == 1 && "$key" == "size" && "$value" == 0x* ]]; then
      section_size=$value
    elif [[ "$in_section" == 1 && "$key" == "offset" ]]; then
      section_offset=$value
    fi
  done < <(otool -l "$file")
  add_section
  sort -nr "$tmp" | awk -F '\t' -v limit="$TOP_COUNT" 'BEGIN { out="" } NR <= limit { if (out != "") out=out ";"; out=out $2 "=" $1 } END { if (out != "") print out; else print "none" }'
  rm -f "$tmp"
}

top_map_entries() {
  local map_file=$1 kind=$2 name_field=$3
  if [[ -z "$map_file" || ! -f "$map_file" ]]; then
    printf 'unavailable'
    return
  fi
  awk -v kind="$kind" -v name_field="$name_field" '$1 == kind && $3 ~ /^[0-9]+$/ && $3 > 0 && $4 !~ /,__bss$/ && $4 !~ /,__common$/ { if (name_field == "input") { source = $5; sub(/^.*\//, "", source); label = $4 ":" source } else { label = $name_field } print $3 "\t" label }' "$map_file" |
    sort -nr |
    awk -F '\t' -v limit="$TOP_COUNT" 'BEGIN { out="" } NR <= limit { if (out != "") out=out ";"; out=out $2 "=" $1 } END { if (out != "") print out; else print "none" }'
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
  printf 'tool\tfile_bytes\tfile_section_bytes\traster_delta\tload_commands\tbuild_tools\tdelta_file_bytes\tdelta_file_section_bytes\ttop_file_sections\ttop_input_sections\ttop_symbols\n'
else
  printf 'tool\tfile_bytes\tfile_section_bytes\traster_delta\tload_commands\tbuild_tools\ttop_file_sections\ttop_input_sections\ttop_symbols\n'
fi
for tool in $TOOLS; do
  file="$BUILD_DIR/$tool"
  map_file=
  if [[ -n "$MAP_DIR" ]]; then
    map_file="$MAP_DIR/$tool.map"
  fi
  if [[ ! -f "$file" ]]; then
    if [[ -n "$COMPARE_FILE" ]]; then
      printf '%s\tmissing\tmissing\tmissing\tmissing\tmissing\tmissing\tmissing\tmissing\tmissing\tmissing\n' "$tool"
    else
      printf '%s\tmissing\tmissing\tmissing\tmissing\tmissing\tmissing\tmissing\tmissing\n' "$tool"
    fi
    continue
  fi
  file_bytes=$(wc -c < "$file" | tr -d ' ')
  sections=$(section_bytes "$file")
  load_summary=$(load_command_summary "$file")
  top_sections=$(top_file_sections "$file")
  top_inputs=$(top_map_entries "$map_file" input-section input)
  top_symbols=$(top_map_entries "$map_file" symbol 5)
  if [[ -n "$COMPARE_FILE" ]]; then
    previous=$(awk -F '\t' -v tool="$tool" 'NR > 1 && $1 == tool { print $2 "\t" $3; exit }' "$COMPARE_FILE")
    if [[ -n "$previous" ]]; then
      previous_file=${previous%%$'\t'*}
      previous_sections=${previous#*$'\t'}
      if [[ "$previous_file" =~ ^[0-9]+$ && "$previous_sections" =~ ^[0-9]+$ ]]; then
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$tool" "$file_bytes" "$sections" "$((file_bytes - sections))" "$load_summary" "$((file_bytes - previous_file))" "$((sections - previous_sections))" "$top_sections" "$top_inputs" "$top_symbols"
      else
        printf '%s\t%s\t%s\t%s\t%s\tmissing\tmissing\t%s\t%s\t%s\n' "$tool" "$file_bytes" "$sections" "$((file_bytes - sections))" "$load_summary" "$top_sections" "$top_inputs" "$top_symbols"
      fi
    else
      printf '%s\t%s\t%s\t%s\t%s\tnew\tnew\t%s\t%s\t%s\n' "$tool" "$file_bytes" "$sections" "$((file_bytes - sections))" "$load_summary" "$top_sections" "$top_inputs" "$top_symbols"
    fi
  else
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$tool" "$file_bytes" "$sections" "$((file_bytes - sections))" "$load_summary" "$top_sections" "$top_inputs" "$top_symbols"
  fi
done