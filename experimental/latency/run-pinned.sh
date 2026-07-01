#!/bin/sh
set -eu

usage() {
    printf '%s\n' "usage: $0 --cpu CPU -- COMMAND [ARG ...]" >&2
}

CPU=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --cpu)
            [ "$#" -ge 2 ] || { usage; exit 2; }
            CPU=$2
            shift 2
            ;;
        --cpu=*)
            CPU=${1#--cpu=}
            shift
            ;;
        --)
            shift
            break
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'run-pinned: unknown option %s\n' "$1" >&2
            usage
            exit 2
            ;;
    esac
done

[ -n "$CPU" ] || { usage; exit 2; }
[ "$#" -gt 0 ] || { usage; exit 2; }

if command -v taskset >/dev/null 2>&1; then
    exec taskset -c "$CPU" "$@"
fi

printf '%s\n' 'run-pinned: taskset not found' >&2
exit 127