#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR=${MACOS_FREESTANDING_BUILD_DIR:-build/freestanding-macos-aarch64}
TOOLS=${TOOLS:-true false cat ncc ssh wget nm size expack}
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
  echo "build it first with make freestanding-macos" >&2
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

if [[ -n "$COMPARE_FILE" ]]; then
  printf 'tool\tfile_bytes\tfile_section_bytes\traster_delta\tdelta_file_bytes\tdelta_file_section_bytes\n'
else
  printf 'tool\tfile_bytes\tfile_section_bytes\traster_delta\n'
fi
for tool in $TOOLS; do
  file="$BUILD_DIR/$tool"
  if [[ ! -f "$file" ]]; then
    if [[ -n "$COMPARE_FILE" ]]; then
      printf '%s\tmissing\tmissing\tmissing\tmissing\tmissing\n' "$tool"
    else
      printf '%s\tmissing\n' "$tool"
    fi
    continue
  fi
  file_bytes=$(wc -c < "$file" | tr -d ' ')
  sections=$(section_bytes "$file")
  if [[ -n "$COMPARE_FILE" ]]; then
    previous=$(awk -F '\t' -v tool="$tool" 'NR > 1 && $1 == tool { print $2 "\t" $3; exit }' "$COMPARE_FILE")
    if [[ -n "$previous" ]]; then
      previous_file=${previous%%$'\t'*}
      previous_sections=${previous#*$'\t'}
      if [[ "$previous_file" =~ ^[0-9]+$ && "$previous_sections" =~ ^[0-9]+$ ]]; then
        printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$tool" "$file_bytes" "$sections" "$((file_bytes - sections))" "$((file_bytes - previous_file))" "$((sections - previous_sections))"
      else
        printf '%s\t%s\t%s\t%s\tmissing\tmissing\n' "$tool" "$file_bytes" "$sections" "$((file_bytes - sections))"
      fi
    else
      printf '%s\t%s\t%s\t%s\tnew\tnew\n' "$tool" "$file_bytes" "$sections" "$((file_bytes - sections))"
    fi
  else
    printf '%s\t%s\t%s\t%s\n' "$tool" "$file_bytes" "$sections" "$((file_bytes - sections))"
  fi
done