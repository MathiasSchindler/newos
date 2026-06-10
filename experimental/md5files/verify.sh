#!/bin/sh
set -eu

hash_kind=${HASH:-md5}
case "$hash_kind" in
    md5|sha1) ;;
    *)
        echo "verify: unsupported HASH=$hash_kind" >&2
        exit 1
        ;;
esac

file_a=${1:-out/${hash_kind}file-a.bin}
file_b=${2:-out/${hash_kind}file-b.bin}
project_hashsum="../../build/${hash_kind}sum"

hash_file() {
    path=$1
    if [ -x "$project_hashsum" ]; then
        "$project_hashsum" "$path" | awk '{print $1}'
        return
    fi
    if [ "$hash_kind" = md5 ] && command -v md5sum >/dev/null 2>&1; then
        md5sum "$path" | awk '{print $1}'
        return
    fi
    if [ "$hash_kind" = md5 ] && command -v md5 >/dev/null 2>&1; then
        md5 -q "$path"
        return
    fi
    if [ "$hash_kind" = sha1 ] && command -v sha1sum >/dev/null 2>&1; then
        sha1sum "$path" | awk '{print $1}'
        return
    fi
    if [ "$hash_kind" = sha1 ] && command -v shasum >/dev/null 2>&1; then
        shasum -a 1 "$path" | awk '{print $1}'
        return
    fi
    echo "verify: no ${hash_kind}sum-compatible command found" >&2
    return 1
}

hash_a=$(hash_file "$file_a")
hash_b=$(hash_file "$file_b")

printf '%s  %s\n' "$hash_a" "$file_a"
printf '%s  %s\n' "$hash_b" "$file_b"

if [ "$hash_a" != "$hash_b" ]; then
    echo "verify: ${hash_kind} digests differ" >&2
    exit 1
fi

if cmp -s "$file_a" "$file_b"; then
    echo "verify: files are identical, expected a collision between different files" >&2
    exit 1
fi

bytes_a=$(wc -c < "$file_a" | tr -d ' ')
bytes_b=$(wc -c < "$file_b" | tr -d ' ')

printf 'same %s: %s\n' "$hash_kind" "$hash_a"
printf 'different files: yes\n'
printf 'sizes: %s bytes, %s bytes\n' "$bytes_a" "$bytes_b"
