# noxis Networking — Design Notes

## Scope

noxis talks to the world through an **NE2000** ISA NIC (QEMU: `-device
ne2k_isa`), which is the classic hobby-OS choice: programmed I/O only, no
PCI enumeration, no busmaster DMA. On top of the driver sits the smallest
useful stack: **Ethernet → ARP → IPv4 → ICMP**, driven by three shell
commands (`PING`, `IFCONFIG`, `ARP`). There is no UDP, TCP or DHCP yet —
those are the natural next steps and the layering below leaves room for
them.

```
  PING / IFCONFIG / ARP        (drivers/shell.c)
          │
  ip.c    ├── ICMP echo req/reply, ping session flags
          ├── IPv4 header, checksum, next-hop choice (gateway vs direct)
  arp.c   ├── ARP cache (8 slots), one outstanding request
          │
  net.c   ├── interface config, statistics, eth frame build/dispatch
          │
  ne2000.c ─── DP8390: reset, PROM read, remote DMA, RX ring, IRQ
```

## QEMU wiring and addresses

```
make run
  = qemu-system-i386 -fda os-image.bin \
      -netdev user,id=net0 -device ne2k_isa,netdev=net0,iobase=0x300,irq=9
```

`-netdev user` is QEMU's slirp: a virtual LAN with **fixed addresses**, so
noxis uses a static configuration instead of DHCP:

| address    | role                              |
|------------|-----------------------------------|
| 10.0.2.15  | the guest (noxis)                 |
| 10.0.2.2   | gateway — answers ARP **and** ICMP echo (try `PING 10.0.2.2`) |
| 10.0.2.3   | virtual DNS server (UDP only)     |

## The driver (`net/ne2000.c`)

The card's 16 KiB SRAM is carved into pages of 256 bytes: `0x40–0x45` stage
outgoing frames, `0x46–0x7F` are the receive ring. Frames move through the
**remote DMA** data port (byte mode, `DCR = 0x48`) — no host DMA engine is
involved, which is exactly why this card is friendly to a teaching kernel.

* **Init**: reset pulse → program the ring (`PSTART/PSTOP/BNRY`), read the
  station address out of the PROM (first 6 bytes via remote read from 0 —
  the read result doubles as card detection: all-0xFF means no card),
  set the MAC filter (`PAR0..5`, page 1), start, unmask the receive
  interrupt (`IMR = 0x01`).
* **TX** (`ne2000_send`): remote-write the frame at `TPSR`, wait for the
  RDC bit, set the byte counts, fire `CR_TXP`, then wait for PTX/TXE so the
  staging buffer is reusable. The whole sequence runs under `cli/sti`
  because the IRQ handler does its own remote DMA.
* **RX** (IRQ 9 → vector 41): walk `BNRY+1 … CURR-1`, remote-read each
  packet's 4-byte card header + payload into a small software ring (8 ×
  1536 B, single-producer/single-consumer, no locking), advance `BNRY`.
  Frames that arrive when the ring is full are counted and dropped.

Protocol processing never happens in interrupt context: `net_poll()`
(called from the `PING` wait loops and the `ARP` command) pops frames off
the software ring and dispatches them by ethertype.

## The protocol layer

* **ARP** (`net/arp.c`): caches every binding it sees; answers *who-has*
  requests for `net_ip`; on a lookup miss sends a request (one outstanding,
  resent after half a second) and returns "try again" — callers park in an
  `hlt` loop polling `net_poll()` until the reply lands or a deadline
  passes.
* **IPv4** (`net/ip.c`): 20-byte header, ones'-complement checksum,
  next-hop selection (`dst ^ net_ip & net_mask` → gateway), TTL 64.
  Incoming packets must be addressed to `net_ip` and checksum-clean.
* **ICMP**: replies to echo requests and records echo replies into the
  `ping_*` session flags (id `0xBEAF`). The `PING` command sends four
  echoes with a one-second timeout each and reports RTT in 20 ms
  timer-tick units — honest resolution for a 50 Hz clock.

## Testing

`tests/qemu_test.py` gained two tests:

* `t8_net_ne2000` — boots with the NIC, asserts `IFCONFIG` output, real
  ARP + ICMP exchange with the slirp gateway (`PING 10.0.2.2`), the ARP
  table, and that the shell survives it all.
* `t9_net_absent_graceful` — boots without the NIC; the commands must
  report the missing card and keep the shell alive.

On hosts without an i386 toolchain both run inside a container:
`tests/docker-test.sh t8_net_ne2000 t9_net_absent_graceful`.

## Ideas to extend

- DHCP discover/offer/request/ack (replace the static `net_ip` setup)
- UDP + a DNS resolver against 10.0.2.3 (`HOST` command?)
- TCP is a much bigger step; start with a simple socket API for kernel
  threads
- checksum offload / ring tuning are pointless here — this stack is for
  learning, not throughput
