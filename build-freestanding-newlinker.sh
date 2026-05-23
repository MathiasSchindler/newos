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
if "$NEWLINKER_CC" --version 2>/dev/null | grep -qi clang; then
  NEWLINKER_TARGET_FLAGS=(-target x86_64-unknown-linux-elf)
  NEWLINKER_NO_ADDRSIG_FLAGS=(-fno-addrsig)
else
  NEWLINKER_TARGET_FLAGS=(-m64)
  NEWLINKER_NO_ADDRSIG_FLAGS=()
fi
if [[ -n "${LINKER_FLAGS:-}" ]]; then
  read -r -a LINKER_FLAGS_ARRAY <<< "$LINKER_FLAGS"
else
  LINKER_FLAGS_ARRAY=("${LINKER_FLAGS_DEFAULT[@]}")
fi
NEWLINKER_EXTRA_CFLAGS=${NEWLINKER_EXTRA_CFLAGS:--fmerge-all-constants}
if [[ -n "${NEWLINKER_EXTRA_CFLAGS:-}" ]]; then
  read -r -a NEWLINKER_EXTRA_CFLAGS_ARRAY <<< "$NEWLINKER_EXTRA_CFLAGS"
else
  NEWLINKER_EXTRA_CFLAGS_ARRAY=()
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

if [[ ! -x "$LINKER" ]]; then
  echo "LINKER_MISSING $LINKER" | tee "$REPORT"
  exit 1
fi

TOOLS=$(awk '/^TOOLS[[:space:]]*:=/{sub(/^[^:]*:=/,""); print; exit}' Makefile)
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
CFLAGS=("${NEWLINKER_TARGET_FLAGS[@]}" -std=c11 -Wall -Wextra -Wpedantic -Oz -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -fno-pic -fno-pie "${NEWLINKER_NO_ADDRSIG_FLAGS[@]}" "${NEWLINKER_EXTRA_CFLAGS_ARRAY[@]}" -DEXPACK_DISABLE_PTHREAD=1 -Isrc/shared -Isrc/compiler -Isrc/platform/posix -Isrc/platform/linux -Isrc/platform/common -Isrc/arch/x86_64/linux)
ASMFLAGS=("${NEWLINKER_TARGET_FLAGS[@]}" -DNEWOS_DISABLE_STACK_GUARD_INIT=1 -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -fno-pic -fno-pie "${NEWLINKER_NO_ADDRSIG_FLAGS[@]}" -Isrc/shared -Isrc/compiler -Isrc/platform/posix -Isrc/platform/linux -Isrc/platform/common -Isrc/arch/x86_64/linux)

declare -A SOURCE_TO_OBJ
obj_for() { local s="$1" variant="${2:-}" n; n="${s//\//__}"; n="${n//[/lb}"; n="${n//]/rb}"; if [[ -n "$variant" ]]; then n="${variant}__${n}"; fi; printf '%s/%s.o' "$OBJROOT" "$n"; }
first_line() { local f="$1"; grep -m1 -E 'error:|undefined reference|multiple definition|unsupported|relocation|failed|cannot|No such file|not found|too many input files|undefined symbol|exceeds' "$f" || sed -n '1p' "$f"; }
tool_stem() { local n="$1"; if [[ "$n" == "[" ]]; then printf 'lbracket'; else n="${n//\//_}"; n="${n//[/lb}"; n="${n//]/rb}"; printf '%s' "$n"; fi; }
compile_one() {
  local src="$1" variant="${2:-}" key obj log rc cflags asmflags
  key="$variant|$src"
  if [[ -n "${SOURCE_TO_OBJ[$key]:-}" ]]; then printf '%s\n' "${SOURCE_TO_OBJ[$key]}"; return 0; fi
  obj=$(obj_for "$src" "$variant"); log="$LOGROOT/compile-${obj##*/}.log"; mkdir -p "$(dirname "$obj")"
  cflags=("${CFLAGS[@]}"); asmflags=("${ASMFLAGS[@]}")
  case "$variant" in
    linker-core) cflags+=(-DCOMPILER_LINKER_ENABLE_REPORTING=0); asmflags+=(-DCOMPILER_LINKER_ENABLE_REPORTING=0) ;;
    linker-report) cflags+=(-DCOMPILER_LINKER_ENABLE_REPORTING=1); asmflags+=(-DCOMPILER_LINKER_ENABLE_REPORTING=1) ;;
  esac
  if [[ "$src" == *.S || "$src" == *.s ]]; then
    "$NEWLINKER_CC" "${asmflags[@]}" -c "$src" -o "$obj" >"$log" 2>&1
  else
    "$NEWLINKER_CC" "${cflags[@]}" -c "$src" -o "$obj" >"$log" 2>&1
  fi
  rc=$?
  if [[ $rc -ne 0 ]]; then echo "$src|$(first_line "$log")"; return 1; fi
  SOURCE_TO_OBJ[$key]="$obj"
  printf '%s\n' "$obj"
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
out=$(compile_one "$CRT_SRC"); if [[ $? -ne 0 ]]; then REUSE_FAILS+=("$out"); else CRT_OBJ="$out"; fi
for src in "${REUSE_SOURCES[@]}"; do
  [[ -f "$src" ]] || continue
  out=$(compile_one "$src")
  if [[ $? -ne 0 ]]; then REUSE_FAILS+=("$out"); else REUSE_OBJS+=("$out"); fi
done
{
  echo "newlinker freestanding build: $WORK"
  echo "compiler: $NEWLINKER_CC"
  echo "linker: $LINKER"
  echo "linker flags: ${LINKER_FLAGS_ARRAY[*]}"
  echo "extra cflags: ${NEWLINKER_EXTRA_CFLAGS_ARRAY[*]:-none}"
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
      ;;
    linker)
      append_unique_source tool_sources src/compiler/linker.c
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
  tool_objs=()
  tfail=""
  for src in "${tool_sources[@]}"; do
    variant=""
    if [[ "$src" == "src/compiler/linker.c" ]]; then
      if [[ "$tool" == "linker" ]]; then variant="linker-report"; elif [[ "$tool" == "ncc" ]]; then variant="linker-core"; fi
    fi
    out=$(compile_one "$src" "$variant")
    if [[ $? -ne 0 ]]; then tfail="$src: ${out#*|}"; break; else tool_objs+=("$out"); fi
  done
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
