#!/bin/sh
set -eu

TOOL=${XSKLAT:-./build/xsklat}
COUNT=${COUNT:-1000}
SIZES=${SIZES:-46 64 128 256 512 1024 1400}
QUEUES=${QUEUES:-0}
TIMEOUT_MS=${TIMEOUT_MS:-1000}
INTERVAL_US=${INTERVAL_US:-0}
ETHERTYPE=${ETHERTYPE:-0x88b5}

usage() {
    printf '%s\n' "usage: $0 -i IFACE --dst MAC [--tool PATH]" >&2
    printf '%s\n' "env: COUNT='$COUNT' SIZES='$SIZES' QUEUES='$QUEUES' TIMEOUT_MS='$TIMEOUT_MS' INTERVAL_US='$INTERVAL_US'" >&2
}

IFACE=
DST=
while [ "$#" -gt 0 ]; do
    case "$1" in
        -i|--iface)
            [ "$#" -ge 2 ] || { usage; exit 2; }
            IFACE=$2
            shift 2
            ;;
        --iface=*)
            IFACE=${1#--iface=}
            shift
            ;;
        --dst)
            [ "$#" -ge 2 ] || { usage; exit 2; }
            DST=$2
            shift 2
            ;;
        --dst=*)
            DST=${1#--dst=}
            shift
            ;;
        --tool)
            [ "$#" -ge 2 ] || { usage; exit 2; }
            TOOL=$2
            shift 2
            ;;
        --tool=*)
            TOOL=${1#--tool=}
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'bench-xsklat: unknown option %s\n' "$1" >&2
            usage
            exit 2
            ;;
    esac
done

[ -n "$IFACE" ] || { usage; exit 2; }
[ -n "$DST" ] || { usage; exit 2; }

printf '# tool=%s iface=%s dst=%s count=%s timeout_ms=%s interval_us=%s ethertype=%s\n' "$TOOL" "$IFACE" "$DST" "$COUNT" "$TIMEOUT_MS" "$INTERVAL_US" "$ETHERTYPE"
for queue in $QUEUES; do
    for size in $SIZES; do
        printf '# case=af_xdp_tx queue=%s size=%s\n' "$queue" "$size"
        if "$TOOL" ping -i "$IFACE" --dst "$DST" --queue "$queue" --count "$COUNT" --size "$size" --timeout-ms "$TIMEOUT_MS" --interval-us "$INTERVAL_US" --ethertype "$ETHERTYPE" --samples; then
            :
        else
            printf '# status=%s\n' "$?"
        fi
    done
done