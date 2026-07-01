#!/bin/sh
set -eu

usage() {
    printf '%s\n' "usage: $0 -i IFACE [--cpu CPU] [--irq-cpu CPU] [--apply]" >&2
    printf '%s\n' "default is dry-run; use --apply to write sysfs/procfs and run ethtool" >&2
}

IFACE=
CPU=0
IRQ_CPU=0
APPLY=0
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
        --cpu)
            [ "$#" -ge 2 ] || { usage; exit 2; }
            CPU=$2
            shift 2
            ;;
        --cpu=*)
            CPU=${1#--cpu=}
            shift
            ;;
        --irq-cpu)
            [ "$#" -ge 2 ] || { usage; exit 2; }
            IRQ_CPU=$2
            shift 2
            ;;
        --irq-cpu=*)
            IRQ_CPU=${1#--irq-cpu=}
            shift
            ;;
        --apply)
            APPLY=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'latency-tune: unknown option %s\n' "$1" >&2
            usage
            exit 2
            ;;
    esac
done

[ -n "$IFACE" ] || { usage; exit 2; }

run() {
    printf '+ %s\n' "$*"
    if [ "$APPLY" = 1 ]; then
        "$@" || true
    fi
}

run_sh() {
    printf '+ %s\n' "$*"
    if [ "$APPLY" = 1 ]; then
        sh -c "$*" || true
    fi
}

if [ "$APPLY" != 1 ]; then
    printf '%s\n' '# dry-run; add --apply to execute these commands'
fi

if command -v ethtool >/dev/null 2>&1; then
    run ethtool -C "$IFACE" adaptive-rx off adaptive-tx off rx-usecs 0 rx-frames 1 tx-usecs 0 tx-frames 1
    run ethtool --set-eee "$IFACE" eee off
else
    printf '%s\n' '# ethtool not found; skipping coalescing and EEE tuning'
fi

for governor in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -e "$governor" ] || continue
    run_sh "printf performance > '$governor'"
done

if [ -r /proc/interrupts ]; then
    for irq in $(awk -v iface="$IFACE" '$0 ~ iface { sub(":", "", $1); print $1 }' /proc/interrupts); do
        if [ -w "/proc/irq/$irq/smp_affinity_list" ] || [ "$APPLY" != 1 ]; then
            run_sh "printf '%s' '$IRQ_CPU' > '/proc/irq/$irq/smp_affinity_list'"
        fi
    done
fi

printf '# suggested benchmark pinning: ./run-pinned.sh --cpu %s -- ./bench-etherlat.sh ...\n' "$CPU"