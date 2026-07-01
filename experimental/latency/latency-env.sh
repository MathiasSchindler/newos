#!/bin/sh
set -eu

usage() {
    printf '%s\n' "usage: $0 -i IFACE" >&2
}

IFACE=
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
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'latency-env: unknown option %s\n' "$1" >&2
            usage
            exit 2
            ;;
    esac
done

[ -n "$IFACE" ] || { usage; exit 2; }

section() {
    printf '\n## %s\n' "$1"
}

read_sysfs() {
    path=$1
    label=$2
    if [ -r "$path" ]; then
        printf '%s\t%s\n' "$label" "$(cat "$path" 2>/dev/null)"
    fi
}

section host
date -Is 2>/dev/null || date
uname -a
printf 'uid\t%s\n' "$(id -u)"

section interface
printf 'iface\t%s\n' "$IFACE"
read_sysfs "/sys/class/net/$IFACE/address" mac
read_sysfs "/sys/class/net/$IFACE/operstate" operstate
read_sysfs "/sys/class/net/$IFACE/mtu" mtu
read_sysfs "/sys/class/net/$IFACE/speed" speed_mbit
read_sysfs "/sys/class/net/$IFACE/duplex" duplex
printf 'driver\t%s\n' "$(basename "$(readlink "/sys/class/net/$IFACE/device/driver" 2>/dev/null || printf unknown)")"

section queues
if [ -d "/sys/class/net/$IFACE/queues" ]; then
    find "/sys/class/net/$IFACE/queues" -maxdepth 1 -type d -name '*-*' -printf '%f\n' 2>/dev/null | sort
fi

section interrupts
if [ -r /proc/interrupts ]; then
    grep -E "(^|[[:space:]])$IFACE($|[-[:space:]])" /proc/interrupts || true
    for irq in $(awk -v iface="$IFACE" '$0 ~ iface { sub(":", "", $1); print $1 }' /proc/interrupts); do
        if [ -r "/proc/irq/$irq/smp_affinity_list" ]; then
            printf 'irq_affinity\t%s\t%s\n' "$irq" "$(cat "/proc/irq/$irq/smp_affinity_list")"
        fi
    done
fi

section cpu_governors
if ls /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor >/dev/null 2>&1; then
    for governor in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        printf '%s\t%s\n' "$(basename "$(dirname "$(dirname "$governor")")")" "$(cat "$governor")"
    done | sort | uniq -c
else
    printf '%s\n' 'no cpufreq governor files visible'
fi

section ethtool
if command -v ethtool >/dev/null 2>&1; then
    ethtool -i "$IFACE" 2>/dev/null || true
    ethtool -c "$IFACE" 2>/dev/null || true
    ethtool --show-eee "$IFACE" 2>/dev/null || true
else
    printf '%s\n' 'ethtool not found'
fi