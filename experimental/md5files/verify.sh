#!/bin/sh
set -eu

file_a=${1:-out/md5file-a.bin}
file_b=${2:-out/md5file-b.bin}
project_md5sum=../../build/md5sum

hash_file() {
    path=$1
    if [ -x "$project_md5sum" ]; then
        "$project_md5sum" "$path" | awk '{print $1}'
        return
    fi
    if command -v md5sum >/dev/null 2>&1; then
        md5sum "$path" | awk '{print $1}'
        return
    fi
    if command -v md5 >/dev/null 2>&1; then
        md5 -q "$path"
        return
    fi
    echo "verify: no md5sum or md5 command found" >&2
    return 1
}

hash_a=$(hash_file "$file_a")
hash_b=$(hash_file "$file_b")

printf '%s  %s\n' "$hash_a" "$file_a"
printf '%s  %s\n' "$hash_b" "$file_b"

if [ "$hash_a" != "$hash_b" ]; then
    echo "verify: MD5 digests differ" >&2
    exit 1
fi

if cmp -s "$file_a" "$file_b"; then
    echo "verify: files are identical, expected a collision between different files" >&2
    exit 1
fi

bytes_a=$(wc -c < "$file_a" | tr -d ' ')
bytes_b=$(wc -c < "$file_b" | tr -d ' ')

printf 'same md5: %s\n' "$hash_a"
printf 'different files: yes\n'
printf 'sizes: %s bytes, %s bytes\n' "$bytes_a" "$bytes_b"