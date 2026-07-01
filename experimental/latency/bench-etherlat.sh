#!/bin/sh
set -eu

TOOL=${ETHERLAT:-./build/etherlat}
COUNT=${COUNT:-1000}
SIZES=${SIZES:-46 64 128 256 512 1024 1400}
BUSY_POLLS=${BUSY_POLLS:-0}
PACKET_MODES=${PACKET_MODES:-raw}
QDISC_BYPASSES=${QDISC_BYPASSES:-0}
TX_RINGS=${TX_RINGS:-0}
RX_RINGS=${RX_RINGS:-0}
TIMEOUT_MS=${TIMEOUT_MS:-1000}
INTERVAL_US=${INTERVAL_US:-0}
ETHERTYPE=${ETHERTYPE:-0x88b5}

usage() {
    printf '%s\n' "usage: $0 -i IFACE [--dst MAC | --discover] [--tool PATH]" >&2
    printf '%s\n' "env: COUNT='$COUNT' SIZES='$SIZES' BUSY_POLLS='$BUSY_POLLS' PACKET_MODES='$PACKET_MODES' QDISC_BYPASSES='$QDISC_BYPASSES' TX_RINGS='$TX_RINGS' RX_RINGS='$RX_RINGS' TIMEOUT_MS='$TIMEOUT_MS' INTERVAL_US='$INTERVAL_US'" >&2
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
for packet_mode in $PACKET_MODES; do
    for qdisc_bypass in $QDISC_BYPASSES; do
        qdisc_arg=
        [ "$qdisc_bypass" = 0 ] || qdisc_arg=--qdisc-bypass
        for tx_ring in $TX_RINGS; do
            tx_ring_arg=
            [ "$tx_ring" = 0 ] || tx_ring_arg=--tx-ring
            for rx_ring in $RX_RINGS; do
                rx_ring_arg=
                [ "$rx_ring" = 0 ] || rx_ring_arg=--rx-ring
                for busy_poll in $BUSY_POLLS; do
                    for size in $SIZES; do
                        printf '# case=af_packet packet_mode=%s qdisc_bypass=%s tx_ring=%s rx_ring=%s size=%s busy_poll_us=%s\n' "$packet_mode" "$qdisc_bypass" "$tx_ring" "$rx_ring" "$size" "$busy_poll"
                        if [ "$busy_poll" = 0 ]; then
                            if "$TOOL" ping -i "$IFACE" --dst "$DST" --count "$COUNT" --size "$size" --timeout-ms "$TIMEOUT_MS" --interval-us "$INTERVAL_US" --ethertype "$ETHERTYPE" --packet-mode "$packet_mode" $qdisc_arg $tx_ring_arg $rx_ring_arg --samples; then
                                :
                            else
                                printf '# status=%s\n' "$?"
                            fi
                        else
                            if "$TOOL" ping -i "$IFACE" --dst "$DST" --count "$COUNT" --size "$size" --timeout-ms "$TIMEOUT_MS" --interval-us "$INTERVAL_US" --ethertype "$ETHERTYPE" --packet-mode "$packet_mode" $qdisc_arg $tx_ring_arg $rx_ring_arg --busy-poll-us "$busy_poll" --samples; then
                                :
                            else
                                printf '# status=%s\n' "$?"
                            fi
                        fi
                    done
                done
            done
        done
    done
done