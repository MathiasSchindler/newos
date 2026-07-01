# Latency experiments

This directory is for local-network latency experiments that keep the project
style: freestanding-first, dependency-free, and no libc for the normal build.

`udplat` is the UDP baseline and `etherlat` is the raw Ethernet slice.
`etherlat` sends a tiny custom protocol directly in Ethernet frames through
Linux `AF_PACKET` raw sockets. It bypasses IP, UDP, TCP, routing, ARP in the
fast path, and socket protocol demux above layer 2. It still uses the NIC,
Ethernet MAC/PHY, Linux driver, DMA, and the kernel packet socket path.

Build:

```sh
make -C experimental/latency freestanding
```

The tool needs `CAP_NET_RAW` to open packet sockets. Prefer granting that one
capability to the local `etherlat` build output over making it setuid root.
`udplat` does not need that capability unless a specific kernel setting requires
extra privilege for `SO_BUSY_POLL`:

```sh
sudo setcap cap_net_raw+ep experimental/latency/build/etherlat
getcap experimental/latency/build/etherlat
```

There is also a convenience target:

```sh
make -C experimental/latency grant-cap
```

Rebuilding replaces the binary and usually clears file capabilities, so rerun
`setcap` after a fresh build. Use `sudo` only as the fallback when file
capabilities are unavailable.

Run the long-lived responder once on the peer machine:

```sh
experimental/latency/build/etherlat listen -i eth0
```

Leave that process running. From this machine, discover the peer MAC over the
same raw Ethernet protocol and then run benchmarks without any more interaction
on the peer:

```sh
experimental/latency/build/etherlat discover -i eth0
./bench-etherlat.sh -i eth0 --discover > build/af-packet.tsv
./bench-latency.sh -i eth0 --discover > build/latency-af-packet.tsv
```

`discover` broadcasts an `etherlat` request and prints the first responder MAC.
The benchmark scripts use the same discovery step when `--discover` is given,
so `--dst MAC` is optional for the one-peer direct-cable case.

For a UDP baseline, start the UDP responder too:

```sh
experimental/latency/build/udplat listen --bind 0.0.0.0 --port 17777
```

For symmetric `SO_BUSY_POLL` runs, put the option on both the responder and
the sender. The tools fail if the kernel rejects the requested socket option,
so a busy-poll benchmark row means the option was actually accepted:

```sh
experimental/latency/build/udplat listen --bind 0.0.0.0 --port 17777 --busy-poll-us 50
experimental/latency/build/etherlat listen -i eth0 --busy-poll-us 50
```

Run benchmarks from the peer, using the responder's IPv4 address and MAC address:

```sh
experimental/latency/build/udplat ping 192.0.2.10 --port 17777 --count 10000 --samples
experimental/latency/build/etherlat ping -i eth0 --dst aa:bb:cc:dd:ee:ff --count 10000 --samples
```

Loopback can be used as a functional smoke test when run as root or with
`CAP_NET_RAW`, but it is not a hardware latency measurement. It exercises the
`AF_PACKET` socket setup, frame parser, reply path, and reporting logic without
NIC DMA, PHY, cable, switch, interrupt moderation, or a peer host:

```sh
experimental/latency/build/etherlat listen -i lo --count 1 --quiet
experimental/latency/build/etherlat discover -i lo --timeout-ms 100 --quiet
experimental/latency/build/etherlat ping -i lo --dst 00:00:00:00:00:00 --count 1 --timeout-ms 100 --quiet
```

The Makefile has loopback smoke targets. `test-udp-loopback` needs no special
privilege; `test-etherlat-loopback` and the combined `test-loopback` target need
`CAP_NET_RAW` on `etherlat`:

```sh
make -C experimental/latency test-udp-loopback
make -C experimental/latency test-etherlat-loopback
make -C experimental/latency test-etherlat-discover-loopback
make -C experimental/latency test-loopback
make -C experimental/latency test-busy-poll-loopback
```

The targets write measured sample rows to `experimental/latency/build/udp-loopback.tsv`
and `experimental/latency/build/loopback.tsv`. The busy-poll controls write to
`experimental/latency/build/udp-busy-poll-loopback.tsv` and
`experimental/latency/build/busy-poll-loopback.tsv`. Override the smoke-test
busy-poll budget with `BUSY_POLL_US=100`. Discovery smoke output is written to
`experimental/latency/build/discover-loopback.txt`.

For more realistic single-host testing without physical hardware, use a veth
pair or network namespaces. That gives each side a real virtual Ethernet MAC,
but still measures the kernel virtual-device path rather than NIC hardware.
Use two machines, or one machine with two physical NICs and a direct cable, for
the measurements that matter most.

Run a payload-size sweep from inside this directory:

```sh
./bench-latency.sh --udp-host 192.0.2.10 -i eth0 --dst aa:bb:cc:dd:ee:ff > build/latency.tsv
./bench-latency.sh --udp-host 192.0.2.10 -i eth0 --discover > build/latency.tsv
./bench-etherlat.sh -i eth0 --dst aa:bb:cc:dd:ee:ff > build/af-packet.tsv
./bench-etherlat.sh -i eth0 --discover > build/af-packet.tsv
```

For a busy-poll sweep:

```sh
BUSY_POLLS="0 25 50 100" COUNT=10000 ./bench-latency.sh --udp-host 192.0.2.10 -i eth0 --dst aa:bb:cc:dd:ee:ff > build/latency-busy-poll.tsv
```

The sweep script applies `--busy-poll-us` to the sender. Start the responder
with the same value when you want a fully symmetric busy-poll comparison.

Useful knobs:

- `--size BYTES` chooses the Ethernet payload size, capped at 1500 bytes.
- `--ethertype HEX` selects the experimental EtherType; the default is `0x88b5`.
- `--busy-poll-us USEC` enables `SO_BUSY_POLL`; a rejected setting is reported as an error.
- `--interval-us USEC` spaces ping frames out for lower offered load.
- `--timeout-ms MS` controls per-packet receive timeout.
- `--samples` emits per-packet TSV rows before the summary.

The initial benchmark ladder is deliberately data-driven:

1. UDP echo baseline, with and without `SO_BUSY_POLL`.
2. `AF_PACKET` raw `sendto`/`recvfrom`, with fixed peer MACs.
3. `AF_PACKET` plus `SO_BUSY_POLL`.
4. `AF_PACKET` mmap rings with `PACKET_RX_RING` and `PACKET_TX_RING`.
5. `AF_XDP` copy mode.
6. `AF_XDP` zero-copy mode on hardware and drivers that support it.

Keep the comparisons on the same hardware and record the kernel/NIC setup:
CPU governor, CPU affinity, IRQ affinity, NIC interrupt coalescing, link speed,
direct cable versus switch, and whether root or `CAP_NET_RAW` is used.