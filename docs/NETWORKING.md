# Networking roadmap

Goal: a shell command `download <url> [file]` that fetches a file over plain HTTP and stores it
on the FAT32 disk. Built in small steps, each one testable on its own in QEMU.

QEMU's user-mode network (`-netdev user`, already used by `build.ps1`) gives the guest a tiny
virtual LAN: the guest is `10.0.2.15`, the router/gateway `10.0.2.2`, DNS `10.0.2.3`, and DHCP
is offered too. No root or host setup is needed.

| Step | What | Test |
| ---- | ---- | ---- |
| 1 | **Done.** NIC transmit, MAC address, fixed receive ring, `net.c` skeleton | `ifconfig`, `nettest` |
| 2 | **Done.** Ethernet + ARP (cache, replies to requests), IPv4 header + checksum, ICMP echo | `ping 10.0.2.2` |
| 3 | **Done.** UDP DNS A-record lookup | internal to `download` |
| 4 | **Done (minimal).** TCP client handshake/ACK/FIN, HTTP/1.0 GET, write response body to FAT32 | `download http://...` |

## Step 1: what exists

* `drivers/rtl8139.c`: `rtl8139_send(frame, len)`, `rtl8139_get_mac()`, `rtl8139_set_rx_handler()`,
  `rtl8139_get_stats()`. RX and TX both run through the interrupt handler.
* `net/net.c`: static config (`10.0.2.15/24`, gw `10.0.2.2`, dns `10.0.2.3`), receive hook that
  counts frames by type, `ifconfig`, and `nettest`.
* Shell: `ifconfig`, `nettest`, `netdebug <on|off>`.

`nettest` broadcasts an ARP "who has 10.0.2.2?" and waits one second for the reply. Success
prints the gateway's MAC (QEMU's is `52:55:0a:00:02:02`) and proves frames go out and come back.

## Step 2: what exists

All in `net/net.c`:

* **ARP:** 8-entry cache, filled from replies and from requests aimed at us; we answer requests
  for `10.0.2.15`. Resolution retries 3 times, 1 s each.
* **IPv4:** 20-byte headers, header checksum verified on receive, DF set on send, unicast-to-us
  only, fragments dropped. Next hop is the host itself when on our subnet, else the gateway.
* **ICMP:** answers echo requests (so the host can ping the guest), matches echo replies by
  id + sequence, reports destination-unreachable and TTL-exceeded errors for the active ping.
* **`ping <ip> [count]`:** round-trip time uses the CPU's TSC (calibrated once against the PIT,
  about 50 ms on first use), so it shows sub-millisecond times instead of 10 ms steps. Pinging
  your own IP or 127.x.x.x is answered locally without touching the NIC.
* **Tests:** `make test-net` builds `net.c` on the host with mocked hardware and a simulated
  gateway (ARP, replies, timeouts, unreachable, corrupt checksums, random garbage frames).

In QEMU user networking, `ping 10.0.2.2` and `ping 10.0.2.3` always work. Pinging the internet
(e.g. `ping 8.8.8.8`) only works if the host lets QEMU send unprivileged ICMP echo
(Linux: `net.ipv4.ping_group_range`); otherwise those pings time out even though the stack is fine.

## Limits to plan for

* **HTTP only.** HTTPS needs TLS, which is out of scope. Test against `python3 -m http.server`
  on the host (reachable from the guest as `10.0.2.2:8000`).
* Redirects (301/302) must be followed, because many `http://` URLs bounce to HTTPS.
* The heap is 1 MiB: stream the body to disk in chunks, never buffer the whole file.
* FAT32 names are 8.3, so downloaded names get truncated.
* Received frames are handled in interrupt context, so protocol handlers must stay short and
  must not call `kprintf` unless `netdebug` is on.

## HTTPS download

`download` now accepts both `http://` and `https://` URLs. HTTPS uses the BearSSL TLS 1.2 client through the existing TCP transport. The build Docker image fetches the header-only BearSSL dependency into `/root/bearssl`.

The initial trust store contains ISRG Root X2, so current Let's Encrypt ECDSA chains rooted at X2 can be verified. The OS currently has no RTC; certificate validity is checked against the build date embedded by the compiler. A broader CA store and a hardware/firmware-backed wall clock are future work.

Example:

```text
download https://valid-isrgrootx2.letsencrypt.org/index.html test.html
```

HTTPS is TLS-encrypted and performs certificate/name validation; it is not a plaintext HTTPS compatibility shim.
