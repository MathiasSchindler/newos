#!/bin/sh
set -eu

UDPLAT=${UDPLAT:-./build/udplat}
ETHERLAT=${ETHERLAT:-./build/etherlat}
COUNT=${COUNT:-1000}
SIZES=${SIZES:-46 64 128 256 512 1024 1400}
BUSY_POLLS=${BUSY_POLLS:-0}
TIMEOUT_MS=${TIMEOUT_MS:-1000}
INTERVAL_US=${INTERVAL_US:-0}
UDP_PORT=${UDP_PORT:-17777}
ETHERTYPE=${ETHERTYPE:-0x88b5}

usage() {
    printf '%s\n' "usage: $0 [--udp-host IPv4] [--iface IFACE [--dst MAC | --discover]]" >&2
    printf '%s\n' "env: COUNT='$COUNT' SIZES='$SIZES' BUSY_POLLS='$BUSY_POLLS' TIMEOUT_MS='$TIMEOUT_MS' INTERVAL_US='$INTERVAL_US' UDP_PORT='$UDP_PORT'" >&2
}

UDP_HOST=
IFACE=
DST=
DISCOVER=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --udp-host)
            [ "$#" -ge 2 ] || { usage; exit 2; }
            UDP_HOST=$2
            shift 2
            ;;
        --udp-host=*)
            UDP_HOST=${1#--udp-host=}
            shift
            ;;
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
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'bench-latency: unknown option %s\n' "$1" >&2
            usage
            exit 2
            ;;
    esac
done

if [ -n "$IFACE" ] && [ -z "$DST" ] && [ "$DISCOVER" = 1 ]; then
    DST=$($ETHERLAT discover -i "$IFACE" --timeout-ms "$TIMEOUT_MS" --ethertype "$ETHERTYPE" --quiet)
fi

if [ -z "$UDP_HOST" ] && { [ -z "$IFACE" ] || [ -z "$DST" ]; }; then
    usage
    exit 2
fi

printf '# count=%s timeout_ms=%s interval_us=%s\n' "$COUNT" "$TIMEOUT_MS" "$INTERVAL_US"
if [ -n "$UDP_HOST" ]; then
    printf '# transport=udp host=%s port=%s\n' "$UDP_HOST" "$UDP_PORT"
    for busy_poll in $BUSY_POLLS; do
        for size in $SIZES; do
            printf '# case=udp size=%s busy_poll_us=%s\n' "$size" "$busy_poll"
            if [ "$busy_poll" = 0 ]; then
                "$UDPLAT" ping "$UDP_HOST" --port "$UDP_PORT" --count "$COUNT" --size "$size" --timeout-ms "$TIMEOUT_MS" --interval-us "$INTERVAL_US" --samples
            else
                "$UDPLAT" ping "$UDP_HOST" --port "$UDP_PORT" --count "$COUNT" --size "$size" --timeout-ms "$TIMEOUT_MS" --interval-us "$INTERVAL_US" --busy-poll-us "$busy_poll" --samples
            fi
        done
    done
fi

if [ -n "$IFACE" ] && [ -n "$DST" ]; then
    printf '# transport=af_packet iface=%s dst=%s ethertype=%s\n' "$IFACE" "$DST" "$ETHERTYPE"
    for busy_poll in $BUSY_POLLS; do
        for size in $SIZES; do
            printf '# case=af_packet_raw size=%s busy_poll_us=%s\n' "$size" "$busy_poll"
            if [ "$busy_poll" = 0 ]; then
                "$ETHERLAT" ping -i "$IFACE" --dst "$DST" --count "$COUNT" --size "$size" --timeout-ms "$TIMEOUT_MS" --interval-us "$INTERVAL_US" --ethertype "$ETHERTYPE" --samples
            else
                "$ETHERLAT" ping -i "$IFACE" --dst "$DST" --count "$COUNT" --size "$size" --timeout-ms "$TIMEOUT_MS" --interval-us "$INTERVAL_US" --ethertype "$ETHERTYPE" --busy-poll-us "$busy_poll" --samples
            fi
        done
    done
fi