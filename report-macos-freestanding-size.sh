#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR=${MACOS_FREESTANDING_BUILD_DIR:-build/freestanding-macos-aarch64}
TOOLS=${TOOLS:-true false cat ncc ssh wget nm size expack}

if [[ ! -d "$BUILD_DIR" ]]; then
  echo "missing macOS freestanding build directory: $BUILD_DIR" >&2
  echo "build it first with make freestanding-macos" >&2
  exit 1
fi
if ! command -v otool >/dev/null 2>&1; then
  echo "missing otool; cannot measure Mach-O section bytes" >&2
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

printf 'tool\tfile_bytes\tfile_section_bytes\traster_delta\n'
for tool in $TOOLS; do
  file="$BUILD_DIR/$tool"
  if [[ ! -f "$file" ]]; then
    printf '%s\tmissing\n' "$tool"
    continue
  fi
  file_bytes=$(wc -c < "$file" | tr -d ' ')
  sections=$(section_bytes "$file")
  printf '%s\t%s\t%s\t%s\n' "$tool" "$file_bytes" "$sections" "$((file_bytes - sections))"
done