# Networking roadmap

Goal: a shell command `download <url> [file]` that fetches a file over plain HTTP and stores it
on the FAT32 disk. Built in small steps, each one testable on its own in QEMU.

QEMU's user-mode network (`-netdev user`, already used by `build.ps1`) gives the guest a tiny
virtual LAN: the guest is `10.0.2.15`, the router/gateway `10.0.2.2`, DNS `10.0.2.3`, and DHCP
is offered too. No root or host setup is needed.

| Step | What | Test |
| ---- | ---- | ---- |
| 1 | **Done.** NIC transmit, MAC address, fixed receive ring, `net.c` skeleton | `ifconfig`, `nettest` |
| 2 | Ethernet + ARP (cache, replies to requests), IPv4 header + checksum, ICMP echo | `ping 10.0.2.2` |
| 3 | UDP, DNS A-record lookup (optionally DHCP) | `nslookup example.com` |
| 4 | TCP client (handshake, seq/ack, retransmit, FIN), HTTP/1.0 GET, write body to FAT32 | `download http://...` |

## Step 1: what exists

* `drivers/rtl8139.c`: `rtl8139_send(frame, len)`, `rtl8139_get_mac()`, `rtl8139_set_rx_handler()`,
  `rtl8139_get_stats()`. RX and TX both run through the interrupt handler.
* `net/net.c`: static config (`10.0.2.15/24`, gw `10.0.2.2`, dns `10.0.2.3`), receive hook that
  counts frames by type, `ifconfig`, and `nettest`.
* Shell: `ifconfig`, `nettest`, `netdebug <on|off>`.

`nettest` broadcasts an ARP "who has 10.0.2.2?" and waits one second for the reply. Success
prints the gateway's MAC (QEMU's is `52:55:0a:00:02:02`) and proves frames go out and come back.

## Limits to plan for

* **HTTP only.** HTTPS needs TLS, which is out of scope. Test against `python3 -m http.server`
  on the host (reachable from the guest as `10.0.2.2:8000`).
* Redirects (301/302) must be followed, because many `http://` URLs bounce to HTTPS.
* The heap is 1 MiB: stream the body to disk in chunks, never buffer the whole file.
* FAT32 names are 8.3, so downloaded names get truncated.
* Received frames are handled in interrupt context, so protocol handlers must stay short and
  must not call `kprintf` unless `netdebug` is on.
