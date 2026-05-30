#!/usr/bin/env bash
set -u
WORK=${WORK:-build/freestanding-linux-newlinker}
OBJROOT="$WORK/.obj"
LOGROOT="$WORK/.logs"
REPORT="$WORK/report.txt"
LINKER=${LINKER:-build/host-linux-x86_64/linker}
LINKER_FLAGS_DEFAULT=(--tiny --gc-sections --icf=safe)
LINKER_REPORTS=${LINKER_REPORTS:-0}
if [[ -z "${NEWLINKER_CC:-}" ]]; then
  if command -v clang >/dev/null 2>&1; then
    NEWLINKER_CC=clang
  elif command -v gcc >/dev/null 2>&1; then
    NEWLINKER_CC=gcc
  else
    NEWLINKER_CC=cc
  fi
fi
NEWLINKER_CC_VERSION=$("$NEWLINKER_CC" --version 2>/dev/null || true)
NEWLINKER_IS_NCC=0
if printf '%s\n' "$NEWLINKER_CC_VERSION" | grep -qi '^ncc\|newos.*ncc'; then
  NEWLINKER_TARGET_FLAGS=(--target linux-x86_64)
  NEWLINKER_NO_ADDRSIG_FLAGS=()
  NEWLINKER_IS_GCC=0
  NEWLINKER_IS_NCC=1
elif printf '%s\n' "$NEWLINKER_CC_VERSION" | grep -qi clang; then
  NEWLINKER_TARGET_FLAGS=(-target x86_64-unknown-linux-elf)
  NEWLINKER_NO_ADDRSIG_FLAGS=(-fno-addrsig)
  NEWLINKER_IS_GCC=0
else
  NEWLINKER_TARGET_FLAGS=(-m64)
  NEWLINKER_NO_ADDRSIG_FLAGS=()
  NEWLINKER_IS_GCC=1
fi
if [[ -n "${LINKER_FLAGS:-}" ]]; then
  read -r -a LINKER_FLAGS_ARRAY <<< "$LINKER_FLAGS"
else
  LINKER_FLAGS_ARRAY=("${LINKER_FLAGS_DEFAULT[@]}")
fi
if [[ -z "${NEWLINKER_EXTRA_CFLAGS+x}" ]]; then
  if [[ "$NEWLINKER_IS_NCC" == "1" ]]; then
    NEWLINKER_EXTRA_CFLAGS=""
  else
    NEWLINKER_EXTRA_CFLAGS="-fmerge-all-constants"
  fi
fi
if [[ -n "${NEWLINKER_EXTRA_CFLAGS:-}" ]]; then
  read -r -a NEWLINKER_EXTRA_CFLAGS_ARRAY <<< "$NEWLINKER_EXTRA_CFLAGS"
else
  NEWLINKER_EXTRA_CFLAGS_ARRAY=()
fi
NEWLINKER_LTO=${NEWLINKER_LTO:-0}
NEWLINKER_PROFILE=${NEWLINKER_PROFILE:-0}
if [[ "$NEWLINKER_LTO" != "1" ]]; then
  for _f in "${NEWLINKER_EXTRA_CFLAGS_ARRAY[@]}"; do
    if [[ "$_f" == "-flto" || "$_f" == -flto=* ]]; then NEWLINKER_LTO=1; break; fi
  done
fi
if [[ "$NEWLINKER_LTO" == "1" && "$NEWLINKER_IS_GCC" == "1" ]]; then
  LINKER_FLAGS_ARRAY+=("--lto-cc=$NEWLINKER_CC")
fi

rm -rf "$WORK"
mkdir -p "$OBJROOT" "$LOGROOT"
if [[ "$LINKER_REPORTS" == "1" ]]; then
  MAPROOT="$WORK/.maps"
  mkdir -p "$MAPROOT"
else
  MAPROOT=""
fi
if [[ -z "${NEWLINKER_LINK_JOBS:-}" ]]; then
  if [[ -n "${PARALLEL_JOBS:-}" ]]; then
    NEWLINKER_LINK_JOBS="$PARALLEL_JOBS"
  elif command -v nproc >/dev/null 2>&1; then
    NEWLINKER_LINK_JOBS=$(nproc)
  else
    NEWLINKER_LINK_JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')
  fi
fi
if ! [[ "$NEWLINKER_LINK_JOBS" =~ ^[0-9]+$ ]] || (( NEWLINKER_LINK_JOBS < 1 )); then
  NEWLINKER_LINK_JOBS=1
fi
if [[ -z "${NEWLINKER_COMPILE_JOBS:-}" ]]; then
  if [[ -n "${PARALLEL_JOBS:-}" ]]; then
    NEWLINKER_COMPILE_JOBS="$PARALLEL_JOBS"
  elif command -v nproc >/dev/null 2>&1; then
    NEWLINKER_COMPILE_JOBS=$(nproc)
  else
    NEWLINKER_COMPILE_JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')
  fi
fi
if ! [[ "$NEWLINKER_COMPILE_JOBS" =~ ^[0-9]+$ ]] || (( NEWLINKER_COMPILE_JOBS < 1 )); then
  NEWLINKER_COMPILE_JOBS=1
fi
NEWLINKER_NCC_BATCH=${NEWLINKER_NCC_BATCH:-auto}

if [[ ! -x "$LINKER" ]]; then
  echo "LINKER_MISSING $LINKER" | tee "$REPORT"
  exit 1
fi

TOOLS=${TOOLS:-$(awk '/^TOOLS[[:space:]]*:=/{sub(/^[^:]*:=/,""); print; exit}' Makefile)}
mapfile -t SHARED_SOURCES < <(grep -oE '"src/shared/(runtime/[^"]+|compression/[^"]+|tool_[^"]+|archive_util|bignum|simple_config|server_log|xml|xml_stream|xml_dtd)\.c"' src/compiler/source_manifest.h | tr -d '"' | sort -u)
mapfile -t PLATFORM_SOURCES < <(grep -oE '"src/platform/linux/[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"' | sort -u)
mapfile -t COMPILER_SOURCES < <(grep -oE '"src/compiler/[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"' | sort -u)
mapfile -t IMAGE_SOURCES < <(grep -oE '"src/shared/(image/[^"]+|crypto/(sha256|p256))\.c"' src/compiler/source_manifest.h | tr -d '"' | sort -u)
mapfile -t CRYPTO_SOURCES < <(grep -oE '"src/shared/crypto/[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"' | sort -u)
mapfile -t TLS_SOURCES < <(grep -oE '"src/shared/tls/[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"' | sort -u)
REUSE_SOURCES=("${SHARED_SOURCES[@]}" "${PLATFORM_SOURCES[@]}" src/arch/x86_64/linux/syscall_stubs.S)
SSH_CRYPTO_SOURCES=("${CRYPTO_SOURCES[@]}" src/shared/crypto/curve25519.c src/shared/crypto/ed25519.c src/shared/crypto/chacha20_poly1305.c src/shared/crypto/ssh_kdf.c)
HASH_SOURCES=(src/shared/hash_util.c "${CRYPTO_SOURCES[@]}")
TLS_PLATFORM_SOURCE=src/platform/linux/tls.c
TUI_SOURCE=src/shared/tui.c
CRT_SRC=src/arch/x86_64/linux/crt0.S
if [[ "$NEWLINKER_IS_NCC" == "1" ]]; then
  CFLAGS=("${NEWLINKER_TARGET_FLAGS[@]}" -std=c11 -Wall -Wextra -Wpedantic -O2 -ffreestanding -ffunction-sections -fdata-sections "${NEWLINKER_EXTRA_CFLAGS_ARRAY[@]}" -DNEWOS_HAVE_PTHREAD=0 -DNEWOS_RUNTIME_THREAD_SAFE_ALLOC=0 -DNEWOS_RUNTIME_ALLOC_LOCK=0 -DEXPACK_DISABLE_PTHREAD=1 -Isrc/shared -Isrc/compiler -Isrc/platform/posix -Isrc/platform/linux -Isrc/platform/common -Isrc/arch/x86_64/linux)
  ASMFLAGS=(-m64 -DNEWOS_DISABLE_STACK_GUARD_INIT=1 -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -fno-pic -fno-pie -Isrc/shared -Isrc/compiler -Isrc/platform/posix -Isrc/platform/linux -Isrc/platform/common -Isrc/arch/x86_64/linux)
else
  CFLAGS=("${NEWLINKER_TARGET_FLAGS[@]}" -std=c11 -Wall -Wextra -Wpedantic -Oz -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -fno-pic -fno-pie "${NEWLINKER_NO_ADDRSIG_FLAGS[@]}" "${NEWLINKER_EXTRA_CFLAGS_ARRAY[@]}" -DEXPACK_DISABLE_PTHREAD=1 -Isrc/shared -Isrc/compiler -Isrc/platform/posix -Isrc/platform/linux -Isrc/platform/common -Isrc/arch/x86_64/linux)
  ASMFLAGS=("${NEWLINKER_TARGET_FLAGS[@]}" -DNEWOS_DISABLE_STACK_GUARD_INIT=1 -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -fno-pic -fno-pie "${NEWLINKER_NO_ADDRSIG_FLAGS[@]}" -Isrc/shared -Isrc/compiler -Isrc/platform/posix -Isrc/platform/linux -Isrc/platform/common -Isrc/arch/x86_64/linux)
fi
if [[ "$NEWLINKER_PROFILE" == "1" || "$NEWLINKER_PROFILE" == "yes" || "$NEWLINKER_PROFILE" == "true" ]]; then
  CFLAGS+=(-finstrument-functions -fno-omit-frame-pointer -fno-inline)
fi
if [[ "$NEWLINKER_LTO" == "1" && "$NEWLINKER_IS_GCC" == "1" ]]; then
  _has_flto=0
  for _f in "${CFLAGS[@]}"; do
    if [[ "$_f" == "-flto" || "$_f" == -flto=* ]]; then _has_flto=1; break; fi
  done
  if [[ $_has_flto -eq 0 ]]; then CFLAGS+=(-flto); fi
fi

declare -A SOURCE_TO_OBJ
obj_for() { local s="$1" variant="${2:-}" n; n="${s//\//__}"; n="${n//[/lb}"; n="${n//]/rb}"; if [[ -n "$variant" ]]; then n="${variant}__${n}"; fi; printf '%s/%s.o' "$OBJROOT" "$n"; }
first_line() { local f="$1"; grep -m1 -E 'error:|undefined reference|multiple definition|unsupported|relocation|failed|cannot|No such file|not found|too many input files|undefined symbol|exceeds' "$f" || sed -n '1p' "$f"; }
tool_stem() { local n="$1"; if [[ "$n" == "[" ]]; then printf 'lbracket'; else n="${n//\//_}"; n="${n//[/lb}"; n="${n//]/rb}"; printf '%s' "$n"; fi; }
variant_for_tool_source() {
  local tool="$1" src="$2"
  if [[ "$src" == "src/compiler/linker.c" || "$src" == "src/compiler/linker_"*.c ]]; then
    if [[ "$tool" == "linker" ]]; then printf 'linker-report'; return 0; fi
    if [[ "$tool" == "ncc" ]]; then printf 'linker-core'; return 0; fi
  fi
  printf ''
}
compile_direct() {
  local src="$1" variant="${2:-}" result="$3" key obj log rc cflags asmflags
  key="$variant|$src"
  obj=$(obj_for "$src" "$variant"); log="$LOGROOT/compile-${obj##*/}.log"; mkdir -p "$(dirname "$obj")"
  cflags=("${CFLAGS[@]}"); asmflags=("${ASMFLAGS[@]}")
  case "$variant" in
    linker-core) cflags+=(-DCOMPILER_LINKER_ENABLE_REPORTING=0); asmflags+=(-DCOMPILER_LINKER_ENABLE_REPORTING=0) ;;
    linker-report) cflags+=(-DCOMPILER_LINKER_ENABLE_REPORTING=1); asmflags+=(-DCOMPILER_LINKER_ENABLE_REPORTING=1) ;;
  esac
  if [[ "$src" == *.S || "$src" == *.s ]]; then
    if [[ "$NEWLINKER_IS_NCC" == "1" ]]; then
      "${NEWLINKER_AS:-cc}" "${asmflags[@]}" -c "$src" -o "$obj" >"$log" 2>&1
    else
      "$NEWLINKER_CC" "${asmflags[@]}" -c "$src" -o "$obj" >"$log" 2>&1
    fi
  else
    "$NEWLINKER_CC" "${cflags[@]}" -c "$src" -o "$obj" >"$log" 2>&1
  fi
  rc=$?
  if [[ $rc -ne 0 ]]; then printf 'fail\t%s\t%s|%s\n' "$key" "$src" "$(first_line "$log")" > "$result"; return 0; fi
  printf 'ok\t%s\t%s\n' "$key" "$obj" > "$result"
  return 0
}
compile_manifest_direct() {
  local variant="$1" manifest="$2" result="$3" log="$4" rc cflags key src obj
  cflags=("${CFLAGS[@]}")
  case "$variant" in
    linker-core) cflags+=(-DCOMPILER_LINKER_ENABLE_REPORTING=0) ;;
    linker-report) cflags+=(-DCOMPILER_LINKER_ENABLE_REPORTING=1) ;;
  esac
  "$NEWLINKER_CC" "${cflags[@]}" -c --compile-manifest "$manifest" >"$log" 2>&1
  rc=$?
  if [[ $rc -ne 0 ]]; then printf 'fail\tmanifest\t%s\n' "$(first_line "$log")" > "$result"; return 0; fi
  : > "$result"
  while IFS=$'\t' read -r src obj; do
    [[ -n "$src" && -n "$obj" ]] || continue
    key="$variant|$src"
    printf 'ok\t%s\t%s\n' "$key" "$obj" >> "$result"
  done < "$manifest"
  return 0
}
compile_sources() {
  local tool="$1"
  local -n sources_ref=$2
  local -n objs_ref=$3
  local resultroot="$WORK/.compile-results/$(tool_stem "${tool:-reuse}")-$$-$RANDOM"
  local pending_results=()
  local pending_manifests=()
  local active=0
  local src variant variant_key key obj result fail message kind result_key result_obj manifest log
  local missing_by_variant=""

  COMPILE_ERROR=""
  mkdir -p "$resultroot"
  for src in "${sources_ref[@]}"; do
    [[ -f "$src" ]] || continue
    variant=$(variant_for_tool_source "$tool" "$src")
    key="$variant|$src"
    if [[ -n "${SOURCE_TO_OBJ[$key]:-}" ]]; then
      continue
    fi
    if [[ "$NEWLINKER_IS_NCC" == "1" && "$src" == *.c && ( "$NEWLINKER_NCC_BATCH" == "1" || "$NEWLINKER_NCC_BATCH" == "yes" || "$NEWLINKER_NCC_BATCH" == "true" || ( "$NEWLINKER_NCC_BATCH" == "auto" && "$NEWLINKER_COMPILE_JOBS" == "1" ) ) ]]; then
      variant_key="${variant:-default}"
      missing_by_variant+="$variant_key"$'\t'"$src"$'\n'
      continue
    fi
    obj=$(obj_for "$src" "$variant")
    result="$resultroot/${#pending_results[@]}-${obj##*/}.tsv"
    compile_direct "$src" "$variant" "$result" &
    pending_results+=("$result")
    active=$((active+1))
    if (( active >= NEWLINKER_COMPILE_JOBS )); then
      wait -n || true
      active=$((active-1))
    fi
  done

  if [[ -n "$missing_by_variant" ]]; then
    while IFS=$'\t' read -r variant_key; do
      [[ -n "$variant_key" ]] || continue
      if [[ "$variant_key" == "default" ]]; then variant=""; else variant="$variant_key"; fi
      manifest="$resultroot/manifest-${#pending_results[@]}-$(tool_stem "$variant_key").tsv"
      log="$LOGROOT/compile-manifest-$(tool_stem "${tool:-reuse}")-$variant_key-${#pending_results[@]}.log"
      result="$resultroot/manifest-${#pending_results[@]}.tsv"
      : > "$manifest"
      while IFS=$'\t' read -r row_variant_key src; do
        [[ "$row_variant_key" == "$variant_key" ]] || continue
        printf '%s\t%s\n' "$src" "$(obj_for "$src" "$variant")" >> "$manifest"
      done <<< "$missing_by_variant"
      compile_manifest_direct "$variant" "$manifest" "$result" "$log" &
      pending_results+=("$result")
      pending_manifests+=("$manifest")
      active=$((active+1))
      if (( active >= NEWLINKER_COMPILE_JOBS )); then
        wait -n || true
        active=$((active-1))
      fi
    done < <(printf '%s' "$missing_by_variant" | cut -f1 | sort -u)
  fi

  while (( active > 0 )); do
    wait -n || true
    active=$((active-1))
  done

  fail=""
  for result in "${pending_results[@]}"; do
    if [[ ! -s "$result" ]]; then
      fail="missing compile result: $result"
      break
    fi
    while IFS=$'\t' read -r kind result_key result_obj; do
      if [[ "$kind" == "ok" ]]; then
        SOURCE_TO_OBJ[$result_key]="$result_obj"
      else
        fail="$result_obj"
        break
      fi
    done < "$result"
    [[ -z "$fail" ]] || break
  done
  if [[ -n "$fail" ]]; then
    COMPILE_ERROR="$fail"
    return 1
  fi

  for src in "${sources_ref[@]}"; do
    [[ -f "$src" ]] || continue
    variant=$(variant_for_tool_source "$tool" "$src")
    key="$variant|$src"
    obj="${SOURCE_TO_OBJ[$key]:-}"
    if [[ -z "$obj" ]]; then
      COMPILE_ERROR="missing object for $src"
      return 1
    fi
    objs_ref+=("$obj")
  done
  return 0
}
append_unique_source() {
  local -n arr=$1
  local src="$2"
  local existing
  [[ -f "$src" ]] || return 0
  for existing in "${arr[@]}"; do [[ "$existing" == "$src" ]] && return 0; done
  arr+=("$src")
}
append_dir_sources() {
  local -n arr=$1
  local dir="$2"
  local src existing found
  [[ -d "$dir" ]] || return 0
  while IFS= read -r src; do
    found=0
    for existing in "${arr[@]}"; do
      if [[ "$existing" == "$src" ]]; then
        found=1
        break
      fi
    done
    if [[ $found -eq 0 ]]; then
      arr+=("$src")
    fi
  done < <(find "$dir" -maxdepth 1 -name '*.c' -print | sort)
}

REUSE_OBJS=()
REUSE_FAILS=()
CRT_SOURCES=("$CRT_SRC")
CRT_OBJS=()
if ! compile_sources "" CRT_SOURCES CRT_OBJS; then REUSE_FAILS+=("$COMPILE_ERROR"); else CRT_OBJ="${CRT_OBJS[0]}"; fi
if ! compile_sources "" REUSE_SOURCES REUSE_OBJS; then REUSE_FAILS+=("$COMPILE_ERROR"); fi
{
  echo "newlinker freestanding build: $WORK"
  echo "compiler: $NEWLINKER_CC"
  if [[ "$NEWLINKER_IS_NCC" == "1" ]]; then echo "ncc mode: 1"; fi
  echo "linker: $LINKER"
  echo "linker flags: ${LINKER_FLAGS_ARRAY[*]}"
  echo "extra cflags: ${NEWLINKER_EXTRA_CFLAGS_ARRAY[*]:-none}"
  echo "profile: $NEWLINKER_PROFILE"
  echo "compile jobs: $NEWLINKER_COMPILE_JOBS"
  echo "ncc batch: $NEWLINKER_NCC_BATCH"
  echo "link jobs: $NEWLINKER_LINK_JOBS"
  echo "tools from Makefile: $(wc -w <<<"$TOOLS")"
  echo "reusable sources attempted: ${#REUSE_SOURCES[@]} plus crt0"
} | tee "$REPORT"
if (( ${#REUSE_FAILS[@]} > 0 )); then
  echo "STOPPED: reusable compile failures: ${#REUSE_FAILS[@]}" | tee -a "$REPORT"
  printf '%s\n' "${REUSE_FAILS[@]}" | sed -n '1,20p' | tee -a "$REPORT"
  exit 1
fi

compile_fail=0; link_fail=0; success=0
FAILFILE="$WORK/failures.tsv"; : > "$FAILFILE"
SUCCESSFILE="$WORK/successes.tsv"; : > "$SUCCESSFILE"
TOOL_OBJROOT="$WORK/.tool-objects"; mkdir -p "$TOOL_OBJROOT"
LINK_RESULTROOT="$WORK/.link-results"; mkdir -p "$LINK_RESULTROOT"
TOOL_QUEUE=()
link_one_tool() {
  local tool="$1" objfile="$2" result="$3" stem outbin llog map_path rc bytes
  local tool_objs=()

  mapfile -t tool_objs < "$objfile"
  stem=$(tool_stem "$tool")
  outbin="$WORK/$tool"
  llog="$LOGROOT/link-$stem.log"
  if [[ "$LINKER_REPORTS" == "1" ]]; then
    map_path="$MAPROOT/$stem.map"
    "$LINKER" "${LINKER_FLAGS_ARRAY[@]}" --stats --map "$map_path" -m x86_64-linux -o "$outbin" "$CRT_OBJ" "${tool_objs[@]}" "${REUSE_OBJS[@]}" >"$llog" 2>&1
  else
    "$LINKER" "${LINKER_FLAGS_ARRAY[@]}" -m x86_64-linux -o "$outbin" "$CRT_OBJ" "${tool_objs[@]}" "${REUSE_OBJS[@]}" >"$llog" 2>&1
  fi
  rc=$?
  if [[ $rc -ne 0 ]]; then
    printf 'link\t%s\t%s\n' "$tool" "$(first_line "$llog")" > "$result"
  else
    if [[ -f "$outbin" ]]; then
      bytes=$(wc -c < "$outbin")
    else
      bytes=0
    fi
    printf 'success\t%s\t%s\n' "$tool" "$bytes" > "$result"
  fi
  return 0
}
for tool in $TOOLS; do
  tool_sources=()
  if [[ -f "src/tools/$tool.c" ]]; then append_unique_source tool_sources "src/tools/$tool.c"
  elif [[ "$tool" == "ping6" && -f src/tools/ping.c ]]; then append_unique_source tool_sources src/tools/ping.c
  elif [[ "$tool" == "[" && -f 'src/tools/[.c' ]]; then append_unique_source tool_sources "src/tools/[.c"
  elif [[ "$tool" == "[" && -f src/tools/test.c ]]; then append_unique_source tool_sources src/tools/test.c
  else compile_fail=$((compile_fail+1)); printf 'compile\t%s\t%s\n' "$tool" "missing main source" >> "$FAILFILE"; continue
  fi
  case "$tool" in
    sql)
      ;;
    expack)
      append_unique_source tool_sources src/shared/crypto/sha256.c
      ;;
    ncc)
      for src in "${COMPILER_SOURCES[@]}"; do append_unique_source tool_sources "$src"; done
      append_unique_source tool_sources src/shared/crypto/sha256.c
      ;;
    linker)
      for src in src/compiler/linker.c src/compiler/linker_util.c src/compiler/linker_elf.c \
                 src/compiler/linker_object.c src/compiler/linker_symbols.c src/compiler/linker_gc.c \
                 src/compiler/linker_merge.c src/compiler/linker_icf.c src/compiler/linker_reloc.c \
                 src/compiler/linker_layout.c src/compiler/linker_report.c src/compiler/linker_lto.c \
                 src/compiler/linker_macho.c src/shared/crypto/sha256.c; do
        append_unique_source tool_sources "$src"
      done
      ;;
    md5sum|sha256sum|sha512sum)
      for src in "${HASH_SOURCES[@]}"; do append_unique_source tool_sources "$src"; done
      ;;
    imginfo|imgcheck|imgmeta|c2pa)
      for src in "${IMAGE_SOURCES[@]}"; do append_unique_source tool_sources "$src"; done
      ;;
    wget|wtf|portscan)
      for src in "${TLS_SOURCES[@]}"; do append_unique_source tool_sources "$src"; done
      for src in "${CRYPTO_SOURCES[@]}"; do append_unique_source tool_sources "$src"; done
      append_unique_source tool_sources "$TLS_PLATFORM_SOURCE"
      ;;
    mail)
      append_dir_sources tool_sources src/tools/mail
      append_unique_source tool_sources "$TUI_SOURCE"
      for src in "${TLS_SOURCES[@]}"; do append_unique_source tool_sources "$src"; done
      for src in "${CRYPTO_SOURCES[@]}"; do append_unique_source tool_sources "$src"; done
      append_unique_source tool_sources "$TLS_PLATFORM_SOURCE"
      ;;
    editor)
      append_dir_sources tool_sources src/tools/editor
      append_unique_source tool_sources "$TUI_SOURCE"
      ;;
    ssh)
      append_dir_sources tool_sources src/tools/ssh
      for src in "${SSH_CRYPTO_SOURCES[@]}"; do append_unique_source tool_sources "$src"; done
      ;;
    sshd)
      append_dir_sources tool_sources src/tools/sshd
      append_unique_source tool_sources src/tools/ssh/ssh_core.c
      append_unique_source tool_sources src/tools/ssh/ssh_client_io.c
      for src in "${SSH_CRYPTO_SOURCES[@]}"; do append_unique_source tool_sources "$src"; done
      ;;
    *)
      append_dir_sources tool_sources "src/tools/$tool"
      ;;
  esac
  if [[ "$NEWLINKER_PROFILE" == "1" || "$NEWLINKER_PROFILE" == "yes" || "$NEWLINKER_PROFILE" == "true" ]]; then
    append_unique_source tool_sources src/platform/linux/profiler_runtime.c
  fi
  tool_objs=()
  tfail=""
  if ! compile_sources "$tool" tool_sources tool_objs; then tfail="$COMPILE_ERROR"; fi
  if [[ -n "$tfail" ]]; then compile_fail=$((compile_fail+1)); printf 'compile\t%s\t%s\n' "$tool" "$tfail" >> "$FAILFILE"; continue; fi
  objfile="$TOOL_OBJROOT/$(tool_stem "$tool").list"
  printf '%s\n' "${tool_objs[@]}" > "$objfile"
  TOOL_QUEUE+=("$tool")
done

active_links=0
for tool in "${TOOL_QUEUE[@]}"; do
  stem=$(tool_stem "$tool")
  link_one_tool "$tool" "$TOOL_OBJROOT/$stem.list" "$LINK_RESULTROOT/$stem.tsv" &
  active_links=$((active_links+1))
  if (( active_links >= NEWLINKER_LINK_JOBS )); then
    wait -n || true
    active_links=$((active_links-1))
  fi
done
while (( active_links > 0 )); do
  wait -n || true
  active_links=$((active_links-1))
done

for tool in "${TOOL_QUEUE[@]}"; do
  stem=$(tool_stem "$tool")
  result="$LINK_RESULTROOT/$stem.tsv"
  if [[ ! -s "$result" ]]; then
    link_fail=$((link_fail+1))
    printf 'link\t%s\t%s\n' "$tool" "missing link result" >> "$FAILFILE"
    continue
  fi
  IFS=$'\t' read -r kind result_tool result_value < "$result"
  if [[ "$kind" == "success" ]]; then
    success=$((success+1))
    printf '%s\t%s\n' "$result_tool" "$result_value" >> "$SUCCESSFILE"
  else
    link_fail=$((link_fail+1))
    cat "$result" >> "$FAILFILE"
  fi
done

{
  echo "RESULT_COUNTS compile_failures=$compile_fail link_failures=$link_fail successes=$success"
  if [[ "$NEWLINKER_LTO" == "1" ]]; then
    if [[ "$NEWLINKER_IS_GCC" == "1" ]]; then
      echo "LTO_MODE: gcc-lto-cc (linker auto-prelinking via --lto-cc=$NEWLINKER_CC)"
    else
      echo "LTO_MODE: lto=1 but compiler is not gcc; prelink not applied"
    fi
  fi
  echo "FIRST_30_FAILURES_GROUPED_BY_ERROR_TYPE:"
  awk -F '\t' '{k=$1 " | " $3; if (!(k in seen)) order[++n]=k; seen[k]=seen[k] ? seen[k] "," $2 : $2; count[k]++} END {if (n==0) print "none"; for (i=1;i<=n && i<=30;i++){k=order[i]; printf "%s | count=%d | tools=%s\n", k, count[k], seen[k]}}' "$FAILFILE"
  echo "SIZE_SUMMARY:"
  if [[ -s "$SUCCESSFILE" ]]; then
    awk -F '\t' '{a[++n]=$2; sum+=$2} END {for(i=1;i<=n;i++){for(j=i+1;j<=n;j++){if(a[j]<a[i]){t=a[i];a[i]=a[j];a[j]=t}}} mid=(n%2?a[(n+1)/2]:(a[n/2]+a[n/2+1])/2); printf "count=%d min=%s median=%s max=%s avg=%.1f\n", n, a[1], mid, a[n], sum/n}' "$SUCCESSFILE"
    for name in true false cat linker ncc ssh wget; do awk -F '\t' -v n="$name" '$1==n {print "size " n "=" $2}' "$SUCCESSFILE"; done
  else
    echo "none"
  fi
} | tee -a "$REPORT"

SMOKE="$WORK/smoke.txt"
: > "$SMOKE"
if [[ -x "$WORK/true" ]]; then "$WORK/true"; echo "true=$?" >> "$SMOKE"; fi
if [[ -x "$WORK/false" ]]; then "$WORK/false"; echo "false=$?" >> "$SMOKE"; fi
if [[ -x "$WORK/cat" ]]; then printf 'abc\n' | "$WORK/cat" > "$WORK/cat.out"; rc=$?; if cmp -s "$WORK/cat.out" <(printf 'abc\n'); then echo "cat=0/$rc" >> "$SMOKE"; else echo "cat=mismatch/$rc" >> "$SMOKE"; fi; fi
if [[ -s "$SMOKE" ]]; then echo "SMOKE:" | tee -a "$REPORT"; cat "$SMOKE" | tee -a "$REPORT"; fi

if (( compile_fail != 0 || link_fail != 0 )); then
  exit 1
fi
