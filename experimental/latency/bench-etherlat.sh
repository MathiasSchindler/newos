#!/bin/sh
set -eu

TOOL=${ETHERLAT:-./build/etherlat}
COUNT=${COUNT:-1000}
SIZES=${SIZES:-46 64 128 256 512 1024 1400}
BUSY_POLLS=${BUSY_POLLS:-0}
TIMEOUT_MS=${TIMEOUT_MS:-1000}
INTERVAL_US=${INTERVAL_US:-0}
ETHERTYPE=${ETHERTYPE:-0x88b5}

usage() {
    printf '%s\n' "usage: $0 -i IFACE [--dst MAC | --discover] [--tool PATH]" >&2
    printf '%s\n' "env: COUNT='$COUNT' SIZES='$SIZES' BUSY_POLLS='$BUSY_POLLS' TIMEOUT_MS='$TIMEOUT_MS' INTERVAL_US='$INTERVAL_US'" >&2
}

IFACE=
DST=
DISCOVER=0
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
        --discover)
            DISCOVER=1
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
            printf 'bench-etherlat: unknown option %s\n' "$1" >&2
            usage
            exit 2
            ;;
    esac
done

[ -n "$IFACE" ] || { usage; exit 2; }
if [ -z "$DST" ]; then
    if [ "$DISCOVER" = 1 ]; then
        DST=$($TOOL discover -i "$IFACE" --timeout-ms "$TIMEOUT_MS" --ethertype "$ETHERTYPE" --quiet)
    else
        usage
        exit 2
    fi
fi

printf '# tool=%s iface=%s dst=%s count=%s timeout_ms=%s interval_us=%s ethertype=%s\n' "$TOOL" "$IFACE" "$DST" "$COUNT" "$TIMEOUT_MS" "$INTERVAL_US" "$ETHERTYPE"
for busy_poll in $BUSY_POLLS; do
    for size in $SIZES; do
        printf '# case=af_packet_raw size=%s busy_poll_us=%s\n' "$size" "$busy_poll"
        if [ "$busy_poll" = 0 ]; then
            "$TOOL" ping -i "$IFACE" --dst "$DST" --count "$COUNT" --size "$size" --timeout-ms "$TIMEOUT_MS" --interval-us "$INTERVAL_US" --ethertype "$ETHERTYPE" --samples
        else
            "$TOOL" ping -i "$IFACE" --dst "$DST" --count "$COUNT" --size "$size" --timeout-ms "$TIMEOUT_MS" --interval-us "$INTERVAL_US" --ethertype "$ETHERTYPE" --busy-poll-us "$busy_poll" --samples
        fi
    done
done