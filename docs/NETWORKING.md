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
| 5 | **Done (minimal).** TLS 1.2 HTTPS client with BearSSL and certificate/name validation | `download https://...` |
| 6 | **Done.** DHCP client; NIC abstraction (`drivers/nic.c`) | `dhcp`, `ifconfig` |
| 7 | **Written, untested on hardware.** RTL8168/8111 driver for real PCs | boot on a PC with that NIC |

## DHCP and NIC drivers

At boot, once interrupts are on, `net_configure()` runs DHCP (DISCOVER, OFFER, REQUEST, ACK;
3 attempts, 2 s each) and takes the address, netmask, router and DNS server from the reply. If
no server answers, the QEMU defaults above stay. `dhcp` reruns it; `ifconfig` shows whether the
address came from DHCP and the lease time. Leases are not renewed yet.

To check DHCP in QEMU, move the virtual LAN off the defaults, e.g.
`-netdev user,id=n0,net=192.168.76.0/24`; the OS should come up as `192.168.76.15`.

`nic_probe_init()` picks the first supported card: RTL8139 (QEMU's `-device rtl8139`), then
RTL8168/8111/8169, the on-board Ethernet of most laptops and desktops. The RTL8168 driver uses
descriptor rings through the I/O-port BAR. Because UEFI machines often don't route the card's
legacy PIC interrupt, its receive ring is also polled from the timer tick. QEMU cannot emulate
this chip, so the driver has not been run yet. Booting on real hardware also needs UEFI boot
and a framebuffer console, which the OS does not have yet.

## Step 1: what exists

* `drivers/rtl8139.c`: `rtl8139_send(frame, len)`, `rtl8139_get_mac()`, `rtl8139_set_rx_handler()`,
  `rtl8139_get_stats()`. RX and TX both run through the interrupt handler.
* `net/net.c`: network config (static QEMU defaults, later DHCP), receive hook that
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

* Downloads buffer the response in memory (up to `DOWNLOAD_MAX`) before writing to disk.
* Redirects (301/302) are not followed automatically.
* A download may buffer up to half the free heap, capped at 16 MiB (the test disk is 32 MiB).
* FAT32 names are 8.3, so downloaded names get truncated.
* Received frames are handled in interrupt context, so protocol handlers must stay short and
  must not call `kprintf` unless `netdebug` is on.

## Download progress

While a transfer is in progress, the shell displays a live progress bar, activity indicator, and the number of body bytes received. When the server provides a valid `Content-Length`, the bar shows a percentage; otherwise it animates while reporting received bytes.

## HTTPS download

`download` accepts both `http://` and `https://` URLs. HTTPS uses the BearSSL TLS 1.2 client through the existing TCP transport. The build Docker image fetches the upstream BearSSL source into `/root/bearssl` and builds its static library for the kernel.

The trust store (`tls/trust_anchors.c`, about 150 public root CAs) is generated from the build image's system CA bundle by `scripts/gen-trust-anchors.sh`; rerun it to refresh the roots:

```text
docker run --rm -v "${PWD}:/root/env" myos-buildenv sh scripts/gen-trust-anchors.sh
```

The TLS buffer is full record size (`BR_SSL_BUFSIZE_MONO`) and lives in `.bss`, not on the 32 KiB boot stack. In TLS mode the TCP receive window advertises the free space in the 32 KiB receive ring, so fast servers cannot overflow it. A failed handshake prints BearSSL's error code (`BR_ERR_*` in `bearssl_ssl.h`/`bearssl_x509.h`; 62 means the certificate chain is not trusted), and a non-200 response prints the HTTP status. Secure entropy requires a CPU with RDRAND; QEMU is launched with `-cpu max` to expose that instruction. Certificate validity is checked against the CMOS real-time clock (`drivers/rtc.c`, read as UTC; `date` shows it in IST, `date -u` in UTC). If the clock is unreadable or set earlier than the build date, the build date is used instead. A clock set too far ahead makes valid certificates look expired (BearSSL error 54).

Example:

```text
download https://valid-isrgrootx2.letsencrypt.org/index.html test.html
```

HTTPS is TLS-encrypted and performs certificate/name validation; it is not a plaintext HTTPS compatibility shim.
