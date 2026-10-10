#include "net/net.h"
#include "sys/smp.h"
#include "net/tcp.h"
#include "net/net_internal.h"
#include "drivers/nic.h"
#include "drivers/timer.h"
#include "lib/print.h"
#include "lib/string.h"
#include "drivers/keyboard.h"
#include "drivers/fat32.h"
#include "drivers/heap.h"
#include "tls/bearssl_client.h"
#include <stdint.h>
#include <stddef.h>

/* CPU primitives. NET_HOST_TEST swaps them for mocks so the protocol code can be
 * unit-tested on the host (see tests/net_host_test.c). */
#ifdef NET_HOST_TEST
extern void     net_test_wait(void);
extern uint64_t net_test_tsc(void);
#define CPU_WAIT()  net_test_wait()
#define READ_TSC()  net_test_tsc()
static inline uint64_t irq_save(void) { return 0; }
static inline void irq_restore(uint64_t f) { (void)f; }
#else
#include "sys/task.h"
#include "sys/process.h"
/* Waiting for the network: let ready programs run, then sleep until the next
 * interrupt (a packet wakes us at once). */
static inline void CPU_WAIT(void) {
    task_yield();
    cpu_wait();
}
static inline uint64_t READ_TSC(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; cli; popq %0" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    __asm__ volatile("pushq %0; popfq" :: "r"(f) : "memory", "cc");
}
#endif

#define IP_PROTO_ICMP 1
#define ICMP_ECHO_REPLY   0
#define ICMP_DEST_UNREACH 3
#define ICMP_ECHO_REQUEST 8
#define ICMP_TIME_EXCEEDED 11
#define PING_PAYLOAD 32

static net_config_t cfg;
static int net_up = 0;
static int cfg_from_dhcp;
static uint32_t cfg_lease_seconds;
static void set_static_config(void);
static volatile int debug = 0;

/* Per-type receive counters (updated from the NIC interrupt). */
static volatile uint32_t rx_arp = 0, rx_ipv4 = 0, rx_other = 0;

/* Set by the RX handler when an ARP reply to our selftest arrives. */
static volatile int selftest_got_reply = 0;
static uint8_t selftest_target_ip[4];
static uint8_t selftest_reply_mac[6];

/* ---- helpers ---------------------------------------------------------- */

static inline uint16_t rd16be(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline void wr32be(uint8_t *p, uint32_t v) { p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static inline void wr16be(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

void net_fmt_mac(char *out, const uint8_t mac[6]) {
    static const char hx[] = "0123456789abcdef";
    int o = 0;
    for (int i = 0; i < 6; i++) {
        out[o++] = hx[mac[i] >> 4];
        out[o++] = hx[mac[i] & 0xF];
        if (i < 5) out[o++] = ':';
    }
    out[o] = '\0';
}

void net_fmt_ip(char *out, const uint8_t ip[4]) {
    int o = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t v = ip[i];
        if (v >= 100) out[o++] = (char)('0' + v / 100);
        if (v >= 10)  out[o++] = (char)('0' + (v / 10) % 10);
        out[o++] = (char)('0' + v % 10);
        if (i < 3) out[o++] = '.';
    }
    out[o] = '\0';
}

/* ---- checksums / IP helpers ------------------------------------------ */

/* Internet checksum (RFC 1071). Over a block that already contains a valid
 * checksum the result is 0. */
static uint16_t inet_checksum(const uint8_t *p, uint32_t len) {
    uint32_t sum = 0;
    while (len > 1) { sum += (uint32_t)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len) sum += (uint32_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static inline int ip_eq(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 4) == 0; }

/* ---- ARP cache -------------------------------------------------------- */

#define ARP_CACHE_SIZE 8
typedef struct { uint8_t ip[4]; uint8_t mac[6]; uint8_t valid; } arp_entry_t;
static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static uint8_t arp_next_slot = 0;

/* Called from the IRQ handler and from normal context; the table is tiny. */
static void arp_cache_put(const uint8_t ip[4], const uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && ip_eq(arp_cache[i].ip, ip)) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    arp_entry_t *e = &arp_cache[arp_next_slot];
    arp_next_slot = (uint8_t)((arp_next_slot + 1) % ARP_CACHE_SIZE);
    memcpy(e->ip, ip, 4);
    memcpy(e->mac, mac, 6);
    e->valid = 1;
}

static int arp_cache_get(const uint8_t ip[4], uint8_t mac[6]) {
    uint64_t fl = irq_save();
    int found = 0;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && ip_eq(arp_cache[i].ip, ip)) {
            memcpy(mac, arp_cache[i].mac, 6);
            found = 1;
            break;
        }
    }
    irq_restore(fl);
    return found;
}

static void arp_build(uint8_t *f, uint16_t op, const uint8_t dst_mac[6],
                      const uint8_t tgt_mac[6], const uint8_t tgt_ip[4]) {
    memcpy(f, dst_mac, 6);
    memcpy(f + 6, cfg.mac, 6);
    wr16be(f + 12, ETHERTYPE_ARP);
    wr16be(f + 14, 1);
    wr16be(f + 16, ETHERTYPE_IPV4);
    f[18] = 6;
    f[19] = 4;
    wr16be(f + 20, op);
    memcpy(f + 22, cfg.mac, 6);
    memcpy(f + 28, cfg.ip, 4);
    memcpy(f + 32, tgt_mac, 6);
    memcpy(f + 38, tgt_ip, 4);
}

static void arp_send_request(const uint8_t target_ip[4]) {
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static const uint8_t zero[6]  = {0, 0, 0, 0, 0, 0};
    uint8_t f[42];
    arp_build(f, 1, bcast, zero, target_ip);
    nic_send(f, sizeof(f));
}

/* ---- ping state (written by the RX handler, read by net_ping) ---------- */

enum { PING_IDLE = 0, PING_GOT_REPLY, PING_GOT_UNREACH, PING_GOT_TTL };

static volatile int      ping_active = 0;
static volatile int      ping_state = PING_IDLE;
static uint16_t          ping_id;
static volatile uint16_t ping_seq;
static uint8_t           ping_dst[4];
static volatile uint64_t ping_rx_tsc;
static volatile uint32_t ping_rx_tick;
static volatile uint8_t  ping_reply_ttl;
static volatile uint16_t ping_reply_bytes;
static volatile uint8_t  ping_err_code;
static uint8_t           ping_err_src[4];

static uint16_t ip_ident = 1;
static uint8_t  echo_reply_buf[NIC_MAX_FRAME];
static void net_rx_download_hook(const uint8_t *frame, uint16_t len);

/* Build Ethernet + IPv4 headers (20 bytes, no options) at f. `payload_len` is
 * the length of what follows the IP header. */
static void ip_build(uint8_t *f, const uint8_t dst_mac[6], const uint8_t dst_ip[4],
                     uint8_t proto, uint16_t payload_len) {
    uint8_t *ip = f + 14;
    memcpy(f, dst_mac, 6);
    memcpy(f + 6, cfg.mac, 6);
    wr16be(f + 12, ETHERTYPE_IPV4);
    ip[0] = 0x45;
    ip[1] = 0;
    wr16be(ip + 2, (uint16_t)(20 + payload_len));
    wr16be(ip + 4, ip_ident++);
    wr16be(ip + 6, 0x4000);              /* Don't Fragment */
    ip[8] = 64;                          /* TTL */
    ip[9] = proto;
    wr16be(ip + 10, 0);
    memcpy(ip + 12, cfg.ip, 4);
    memcpy(ip + 16, dst_ip, 4);
    wr16be(ip + 10, inet_checksum(ip, 20));
}

/* ---- receive path (runs in IRQ context: keep it short) ---------------- */

static void handle_arp(const uint8_t *frame, uint16_t len) {
    if (len < 42) return;
    if (rd16be(frame + 14) != 1 || rd16be(frame + 16) != ETHERTYPE_IPV4 ||
        frame[18] != 6 || frame[19] != 4) return;

    uint16_t op = rd16be(frame + 20);
    const uint8_t *smac = frame + 22, *sip = frame + 28, *tip = frame + 38;
    int for_us = ip_eq(tip, cfg.ip);

    /* Learn from replies, and from requests aimed at us. Never learn our own address. */
    if ((op == 2 || for_us) && !ip_eq(sip, cfg.ip) && !ip_eq(sip, (const uint8_t[4]){0, 0, 0, 0}))
        arp_cache_put(sip, smac);

    if (op == 1 && for_us) {
        uint8_t f[42];
        arp_build(f, 2, smac, smac, sip);
        nic_send(f, sizeof(f));
    }

    /* Selftest: ARP reply whose sender IP is the one `nettest` asked about. */
    if (op == 2 && ip_eq(sip, selftest_target_ip)) {
        memcpy(selftest_reply_mac, smac, 6);
        selftest_got_reply = 1;
    }
}

static void handle_icmp(const uint8_t *frame, const uint8_t *ip, uint16_t ihl, uint16_t total) {
    const uint8_t *icmp = ip + ihl;
    uint16_t icmp_len = (uint16_t)(total - ihl);
    if (icmp_len < 8) return;
    if (inet_checksum(icmp, icmp_len) != 0) return;   /* corrupt: drop */

    uint8_t type = icmp[0];

    if (type == ICMP_ECHO_REQUEST) {
        /* Answer pings aimed at us. */
        if (14 + 20 + icmp_len > NIC_MAX_FRAME) return;
        uint8_t *f = echo_reply_buf;
        ip_build(f, frame + 6, ip + 12, IP_PROTO_ICMP, icmp_len);
        uint8_t *r = f + 14 + 20;
        memcpy(r, icmp, icmp_len);
        r[0] = ICMP_ECHO_REPLY;
        wr16be(r + 2, 0);
        wr16be(r + 2, inet_checksum(r, icmp_len));
        nic_send(f, (uint16_t)(14 + 20 + icmp_len));
        return;
    }

    if (!ping_active || ping_state != PING_IDLE) return;

    if (type == ICMP_ECHO_REPLY) {
        if (rd16be(icmp + 4) == ping_id && rd16be(icmp + 6) == ping_seq &&
            ip_eq(ip + 12, ping_dst)) {
            ping_rx_tsc = READ_TSC();
            ping_rx_tick = get_tick();
            ping_reply_ttl = ip[8];
            ping_reply_bytes = (uint16_t)(icmp_len - 8);
            ping_state = PING_GOT_REPLY;
        }
    } else if (type == ICMP_DEST_UNREACH || type == ICMP_TIME_EXCEEDED) {
        /* Body = the IP header + first 8 bytes of the datagram that caused it. */
        if (icmp_len < 8 + 20 + 8) return;
        const uint8_t *in = icmp + 8;
        uint16_t in_ihl = (uint16_t)((in[0] & 0x0F) * 4);
        if ((in[0] >> 4) != 4 || in_ihl < 20 || icmp_len < 8 + in_ihl + 8) return;
        if (in[9] != IP_PROTO_ICMP) return;
        const uint8_t *in_icmp = in + in_ihl;
        if (in_icmp[0] == ICMP_ECHO_REQUEST && rd16be(in_icmp + 4) == ping_id &&
            rd16be(in_icmp + 6) == ping_seq) {
            memcpy(ping_err_src, ip + 12, 4);
            ping_err_code = icmp[1];
            ping_state = (type == ICMP_DEST_UNREACH) ? PING_GOT_UNREACH : PING_GOT_TTL;
        }
    }
}

static void handle_ipv4(const uint8_t *frame, uint16_t len) {
    if (len < 14 + 20) return;
    const uint8_t *ip = frame + 14;
    uint16_t ihl = (uint16_t)((ip[0] & 0x0F) * 4);
    uint16_t total = rd16be(ip + 2);
    if ((ip[0] >> 4) != 4 || ihl < 20 || total < ihl || total > len - 14) return;
    if (inet_checksum(ip, ihl) != 0) return;
    if (!ip_eq(ip + 16, cfg.ip)) return;                 /* unicast to us only */
    if (rd16be(ip + 6) & 0x3FFF) return;                 /* fragments: not supported */
    if (ip[9] == IP_PROTO_ICMP) handle_icmp(frame, ip, ihl, total);
}

static void net_rx(const uint8_t *frame, uint16_t len) {
    if (len < 14) return;
    uint16_t type = rd16be(frame + 12);

    if (type == ETHERTYPE_ARP) rx_arp++;
    else if (type == ETHERTYPE_IPV4) rx_ipv4++;
    else rx_other++;

    if (debug) {
        char smac[18];
        net_fmt_mac(smac, frame + 6);
        kprintf("[NET] rx %u bytes from %s type 0x%x\n", (uint32_t)len, smac, (uint32_t)type);
    }

    if (type == ETHERTYPE_ARP) handle_arp(frame, len);
    else if (type == ETHERTYPE_IPV4) handle_ipv4(frame, len);
}

/* ---- public API ------------------------------------------------------- */

void net_init(void) {
    if (!nic_is_up()) return;

    nic_get_mac(cfg.mac);
    set_static_config();   /* QEMU defaults until net_configure() runs DHCP */

    nic_set_rx_handler(net_rx_download_hook);
    net_up = 1;
#ifndef NET_HOST_TEST
    timer_add_poll_hook(tcp_tick);                 /* resends and TIME-WAIT */
    tcp_set_interrupt_check(process_interrupted);  /* Ctrl+C stops a waiting socket call */
#endif

    char m[18];
    net_fmt_mac(m, cfg.mac);
    kprintf("[NET] interface up, MAC %s\n", m);
}

int net_is_up(void) {
    return net_up;
}

uint32_t net_lease_seconds(void) {
    return cfg_from_dhcp ? cfg_lease_seconds : 0;
}

const net_config_t *net_get_config(void) {
    return &cfg;
}

void net_set_debug(int on) {
    debug = on;
}

void net_print_ifconfig(void) {
    if (!net_up) {
        print_str("No network interface (is the NIC attached? QEMU needs -device rtl8139 or e1000; real PCs need an RTL8168; VirtualBox an Intel PRO/1000)\n");
        return;
    }

    char buf[18];
    kprintf("eth0 (%s)\n", nic_name());
    net_fmt_mac(buf, cfg.mac);       kprintf("  MAC      %s\n", buf);
    net_fmt_ip(buf, cfg.ip);
    if (cfg_from_dhcp) kprintf("  IP       %s  (DHCP, lease %u s)\n", buf, cfg_lease_seconds);
    else               kprintf("  IP       %s  (static)\n", buf);
    net_fmt_ip(buf, cfg.netmask);    kprintf("  Netmask  %s\n", buf);
    net_fmt_ip(buf, cfg.gateway);    kprintf("  Gateway  %s\n", buf);
    net_fmt_ip(buf, cfg.dns);        kprintf("  DNS      %s\n", buf);

    nic_stats_t st;
    nic_get_stats(&st);
    kprintf("  RX       %u packets, %u bytes, %u errors\n", st.rx_packets, st.rx_bytes, st.rx_errors);
    kprintf("           ARP %u, IPv4 %u, other %u\n", (uint32_t)rx_arp, (uint32_t)rx_ipv4, (uint32_t)rx_other);
    kprintf("  TX       %u packets, %u bytes, %u errors\n", st.tx_packets, st.tx_bytes, st.tx_errors);
}

/* Send one broadcast ARP "who has <gateway>?" and wait up to one second for
 * the reply. This exercises TX, the wire, and RX end to end. */
int net_selftest(void) {
    if (!net_up) {
        print_str("No network interface\n");
        return -1;
    }

    uint8_t f[42];
    memcpy(selftest_target_ip, cfg.gateway, 4);
    selftest_got_reply = 0;

    /* Ethernet header: broadcast, our MAC, ARP */
    for (int i = 0; i < 6; i++) f[i] = 0xFF;
    memcpy(f + 6, cfg.mac, 6);
    wr16be(f + 12, ETHERTYPE_ARP);
    /* ARP: Ethernet/IPv4, request */
    wr16be(f + 14, 1);               /* hardware type: Ethernet */
    wr16be(f + 16, ETHERTYPE_IPV4);  /* protocol type */
    f[18] = 6;                       /* hardware address length */
    f[19] = 4;                       /* protocol address length */
    wr16be(f + 20, 1);               /* opcode: request */
    memcpy(f + 22, cfg.mac, 6);      /* sender MAC */
    memcpy(f + 28, cfg.ip, 4);       /* sender IP */
    for (int i = 0; i < 6; i++) f[32 + i] = 0;   /* target MAC: unknown */
    memcpy(f + 38, cfg.gateway, 4);  /* target IP */

    char ipbuf[16];
    net_fmt_ip(ipbuf, cfg.gateway);
    kprintf("Sending ARP request: who has %s? ", ipbuf);

    if (nic_send(f, sizeof(f)) != 0) {
        print_str("\nTX failed\n");
        return -1;
    }

    /* The shell runs with interrupts enabled, so sleep() (hlt) wakes on both
     * the timer and the NIC interrupt. */
    for (int waited = 0; waited < 1000 && !selftest_got_reply; waited += 10) {
        sleep(10);
    }

    if (selftest_got_reply) {
        char m[18];
        net_fmt_mac(m, selftest_reply_mac);
        kprintf("reply from %s\nTX and RX both work.\n", m);
        return 0;
    }
    print_str("no reply\nTX or RX is not working (try `netdebug on`, run QEMU with -serial stdio).\n");
    return -1;
}

/* ---- ping ------------------------------------------------------------- */

int net_parse_ip(const char *s, uint8_t out[4]) {
    if (!s) return -1;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return -1;
        uint32_t v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s - '0');
            if (++digits > 3 || v > 255) return -1;
            s++;
        }
        out[part] = (uint8_t)v;
        if (part < 3) {
            if (*s != '.') return -1;
            s++;
        }
    }
    return *s == '\0' ? 0 : -1;
}

/* TSC calibration against the PIT, done once, on first use (~50 ms). Gives
 * sub-millisecond round-trip times; the PIT alone only ticks every 10 ms. */
static uint64_t tsc_khz = 0;

static void calibrate_tsc(void) {
    if (tsc_khz) return;
    uint32_t t = get_tick();
    while (get_tick() == t) CPU_WAIT();          /* align to a tick edge */
    uint32_t t0 = get_tick();
    uint64_t c0 = READ_TSC();
    while ((uint32_t)(get_tick() - t0) < 5) CPU_WAIT();
    uint64_t c1 = READ_TSC();
    uint32_t elapsed_ms = 5 * 1000 / TIMER_FREQ;
    tsc_khz = (c1 - c0) / elapsed_ms;
}

/* "12.34" from microseconds (buffer >= 16 bytes). */
static void fmt_us(char *out, uint32_t us) {
    uint32_t whole = us / 1000, frac = (us % 1000) / 10;
    char tmp[11];
    int n = 0, o = 0;
    do { tmp[n++] = (char)('0' + whole % 10); whole /= 10; } while (whole && n < 10);
    while (n) out[o++] = tmp[--n];
    out[o++] = '.';
    out[o++] = (char)('0' + frac / 10);
    out[o++] = (char)('0' + frac % 10);
    out[o] = '\0';
}

static uint32_t tsc_to_us(uint64_t cycles) {
    if (!tsc_khz) return 0;
    return (uint32_t)((cycles * 1000) / tsc_khz);
}

/* Find the MAC to put in the Ethernet header for `dst`: the host itself when it
 * is on our subnet, otherwise the gateway. Sends ARP requests until it answers. */
static int resolve_next_hop(const uint8_t dst[4], uint8_t mac[6]) {
    int on_link = 1;
    for (int i = 0; i < 4; i++)
        if ((dst[i] & cfg.netmask[i]) != (cfg.ip[i] & cfg.netmask[i])) on_link = 0;
    const uint8_t *hop = on_link ? dst : cfg.gateway;

    for (int attempt = 0; attempt < 3; attempt++) {
        if (arp_cache_get(hop, mac)) return 0;
        arp_send_request(hop);
        for (int waited = 0; waited < 1000; waited += 10) {
            if (arp_cache_get(hop, mac)) return 0;
            sleep(10);
        }
    }
    return arp_cache_get(hop, mac) ? 0 : -1;
}

static int key_pressed(void) {
    return get_char() != 0;
}

int net_ping(const uint8_t ip[4], uint32_t count) {
    if (!net_up) {
        print_str("No network interface (is the NIC attached? QEMU needs -device rtl8139 or e1000; real PCs need an RTL8168; VirtualBox an Intel PRO/1000)\n");
        return PING_NET_DOWN;
    }
    if (count == 0) count = 4;

    char ipstr[16];
    net_fmt_ip(ipstr, ip);

    int loopback = ip_eq(ip, cfg.ip) || ip[0] == 127;

    uint8_t dst_mac[6];
    if (!loopback) {
        if (resolve_next_hop(ip, dst_mac) != 0) {
            kprintf("ping: cannot reach %s (no ARP reply from the next hop)\n", ipstr);
            return PING_NO_ROUTE;
        }
        calibrate_tsc();
    }

    kprintf("PING %s: %u data bytes (press any key to stop)\n", ipstr, (uint32_t)PING_PAYLOAD);

    memcpy(ping_dst, ip, 4);
    ping_id = (uint16_t)(0x4F53 ^ (get_tick() & 0xFFFF));

    uint32_t sent = 0, received = 0;
    uint32_t rtt_min = 0xFFFFFFFF, rtt_max = 0;
    uint64_t rtt_sum = 0;

    for (uint32_t seq = 1; seq <= count; seq++) {
        if (key_pressed()) break;

        uint32_t rtt_us = 0;
        int ok = 0;

        if (loopback) {
            /* Traffic to ourselves never reaches the NIC. */
            sent++;
            ok = 1;
            kprintf("%u bytes from %s: icmp_seq=%u ttl=64 time=<0.01 ms\n",
                    (uint32_t)PING_PAYLOAD, ipstr, seq);
        } else {
            uint8_t f[14 + 20 + 8 + PING_PAYLOAD];
            uint8_t *icmp = f + 14 + 20;
            icmp[0] = ICMP_ECHO_REQUEST;
            icmp[1] = 0;
            wr16be(icmp + 2, 0);
            wr16be(icmp + 4, ping_id);
            wr16be(icmp + 6, (uint16_t)seq);
            for (int i = 0; i < PING_PAYLOAD; i++) icmp[8 + i] = (uint8_t)('a' + (i % 23));
            wr16be(icmp + 2, inet_checksum(icmp, 8 + PING_PAYLOAD));
            ip_build(f, dst_mac, ip, IP_PROTO_ICMP, 8 + PING_PAYLOAD);

            ping_seq = (uint16_t)seq;
            ping_state = PING_IDLE;
            ping_active = 1;

            uint32_t tick0 = get_tick();
            uint64_t tsc0 = READ_TSC();
            sent++;

            if (nic_send(f, sizeof(f)) != 0) {
                ping_active = 0;
                kprintf("icmp_seq=%u: send failed\n", seq);
            } else {
                /* Wait for the reply, up to one second. */
                while (ping_state == PING_IDLE && (uint32_t)(get_tick() - tick0) < TIMER_FREQ)
                    CPU_WAIT();
                ping_active = 0;

                int st = ping_state;
                if (st == PING_GOT_REPLY) {
                    ok = 1;
                    rtt_us = tsc_khz ? tsc_to_us(ping_rx_tsc - tsc0)
                                     : (uint32_t)(ping_rx_tick - tick0) * (1000000 / TIMER_FREQ);
                    char t[16];
                    fmt_us(t, rtt_us);
                    kprintf("%u bytes from %s: icmp_seq=%u ttl=%u time=%s ms\n",
                            (uint32_t)ping_reply_bytes, ipstr, seq, (uint32_t)ping_reply_ttl, t);
                } else if (st == PING_GOT_UNREACH || st == PING_GOT_TTL) {
                    char from[16];
                    net_fmt_ip(from, ping_err_src);
                    if (st == PING_GOT_TTL) {
                        kprintf("From %s: icmp_seq=%u Time to live exceeded\n", from, seq);
                    } else {
                        const char *why = ping_err_code == 0 ? "Destination Net Unreachable"
                                        : ping_err_code == 1 ? "Destination Host Unreachable"
                                        : ping_err_code == 3 ? "Destination Port Unreachable"
                                        : "Destination Unreachable";
                        kprintf("From %s: icmp_seq=%u %s\n", from, seq, why);
                    }
                } else {
                    kprintf("Request timeout for icmp_seq=%u\n", seq);
                }
            }

            /* Keep to one request per second: wait out the rest of the interval. */
            if (seq < count) {
                while ((uint32_t)(get_tick() - tick0) < TIMER_FREQ) {
                    if (key_pressed()) { count = seq; break; }
                    CPU_WAIT();
                }
            }
        }

        if (ok) {
            received++;
            if (!loopback) {
                if (rtt_us < rtt_min) rtt_min = rtt_us;
                if (rtt_us > rtt_max) rtt_max = rtt_us;
                rtt_sum += rtt_us;
            }
        }
        if (loopback && seq < count) sleep(1000);
    }

    uint32_t loss = sent ? (100 * (sent - received)) / sent : 0;
    kprintf("\n--- %s ping statistics ---\n", ipstr);
    kprintf("%u packets transmitted, %u received, %u%% packet loss\n", sent, received, loss);
    if (received && !loopback) {
        char a[16], b[16], c[16];
        fmt_us(a, rtt_min);
        fmt_us(b, (uint32_t)(rtt_sum / received));
        fmt_us(c, rtt_max);
        kprintf("rtt min/avg/max = %s/%s/%s ms\n", a, b, c);
    }
    return received ? PING_OK : PING_NO_REPLY;
}


/* ---- tiny DNS + TCP + HTTP client ------------------------------------
 *
 * This is deliberately a small client rather than a general socket stack:
 * one DNS A lookup, one TCP connection, HTTP/1.0 GET, and connection close.
 * It is enough for `download http://...` in QEMU user networking.
 */

#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17
#define DNS_PORT 53
#define TCP_MSS 1460
/* Largest body one download may buffer in memory: half the free heap, between
 * 512 KiB and 16 MiB (the FAT32 test disk is 32 MiB). Set before each download. */
static uint32_t download_max = 512u * 1024u;
#define DOWNLOAD_MAX download_max
static void set_download_max(void) {
    uint64_t half = heap_get_free() / 2;
    if (half > 16u * 1024u * 1024u) half = 16u * 1024u * 1024u;
    if (half < 512u * 1024u) half = 512u * 1024u;
    download_max = (uint32_t)half;
}
static volatile int dns_done;
static volatile int dns_failed;
static uint16_t dns_id;
static uint16_t dns_port;
static uint8_t dns_answer[4];

static int download_progress_active;
static uint32_t download_progress_received;
static uint32_t download_progress_total;

static int header_name_matches(const char *line, uint32_t line_len, const char *name) {
    uint32_t i = 0;
    while (name[i]) {
        if (i >= line_len) return 0;
        char c = line[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != name[i]) return 0;
        i++;
    }
    return i < line_len && line[i] == ':';
}

static int parse_content_length(const char *headers, uint32_t length, uint32_t *out) {
    uint32_t pos = 0;
    while (pos < length) {
        uint32_t end = pos;
        while (end + 1 < length && !(headers[end] == '\r' && headers[end + 1] == '\n')) end++;
        if (end + 1 >= length) break;
        uint32_t line_len = end - pos;
        if (header_name_matches(headers + pos, line_len, "content-length")) {
            uint32_t i = pos;
            while (i < end && headers[i] != ':') i++;
            i++;
            while (i < end && (headers[i] == ' ' || headers[i] == '\t')) i++;
            if (i == end || headers[i] < '0' || headers[i] > '9') return 0;
            uint32_t value = 0;
            while (i < end && headers[i] >= '0' && headers[i] <= '9') {
                uint32_t digit = (uint32_t)(headers[i] - '0');
                if (value > (0xFFFFFFFFu - digit) / 10u) return 0;
                value = value * 10u + digit;
                i++;
            }
            while (i < end && (headers[i] == ' ' || headers[i] == '\t')) i++;
            if (i != end) return 0;
            *out = value;
            return 1;
        }
        pos = end + 2;
    }
    return 0;
}

static void append_progress_number(char *line, uint32_t *length, uint32_t value) {
    char digits[10];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && count < sizeof(digits));
    while (count) line[(*length)++] = digits[--count];
}

void net_download_progress(uint32_t received, uint32_t total) {
    char line[64];
    uint32_t length = 0;
    static uint32_t unknown_step;
    static uint32_t activity_step;
    static const char activity[] = "|/-\\";

    download_progress_received = received;
    download_progress_total = total;

    if (download_progress_active) {
        for (uint32_t i = 0; i < sizeof(line) - 1; i++) print_str("\b");
    }
    download_progress_active = 1;

    const char *label = "Downloading [";
    while (*label) line[length++] = *label++;
    uint32_t filled;
    if (total) {
        uint32_t bounded = received > total ? total : received;
        filled = (bounded * 20u) / total;
    } else {
        filled = unknown_step++ % 20u;
    }
    for (uint32_t i = 0; i < 20; i++) {
        if (total) line[length++] = i < filled ? '#' : '.';
        else line[length++] = i == filled ? '>' : '.';
    }
    line[length++] = ']';
    line[length++] = ' ';
    if (total) {
        uint32_t percent = received >= total ? 100u : (received * 100u) / total;
        if (percent < 100) line[length++] = ' ';
        if (percent < 10) line[length++] = ' ';
        append_progress_number(line, &length, percent);
        line[length++] = '%';
        line[length++] = ' ';
    }
    append_progress_number(line, &length, received);
    const char *suffix = " bytes";
    while (*suffix) line[length++] = *suffix++;
    line[length++] = ' ';
    line[length++] = activity[activity_step++ % (sizeof(activity) - 1)];
    while (length < sizeof(line) - 1) line[length++] = ' ';
    line[length] = '\0';
    print_str(line);
}

static void finish_download_progress(void) {
    if (download_progress_active) {
        print_str("\n");
        download_progress_active = 0;
    }
}

static uint16_t pseudo_checksum(const uint8_t src[4], const uint8_t dst[4],
                                uint8_t proto, const uint8_t *data, uint16_t len) {
    uint32_t sum = 0;
    for (int i = 0; i < 4; i += 2) sum += (src[i] << 8) | src[i + 1];
    for (int i = 0; i < 4; i += 2) sum += (dst[i] << 8) | dst[i + 1];
    sum += proto;
    sum += len;
    const uint8_t *p = data;
    while (len > 1) { sum += (p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) sum += p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static void udp_build(uint8_t *f, const uint8_t dst_mac[6], const uint8_t dst_ip[4],
                      uint16_t sport, uint16_t dport, const uint8_t *payload, uint16_t plen) {
    ip_build(f, dst_mac, dst_ip, IP_PROTO_UDP, (uint16_t)(8 + plen));
    uint8_t *u = f + 34;
    wr16be(u, sport); wr16be(u + 2, dport); wr16be(u + 4, (uint16_t)(8 + plen));
    wr16be(u + 6, 0); /* UDP checksum 0 is legal for IPv4. */
    memcpy(u + 8, payload, plen);
}

static void dns_rx(const uint8_t *udp, uint16_t len) {
    if (len < 12 || rd16be(udp) != dns_id || rd16be(udp + 6) == 0) return;
    uint16_t qd = rd16be(udp + 4), an = rd16be(udp + 6);
    if (qd != 1 || an == 0) { dns_failed = 1; return; }
    uint32_t p = 12;
    while (p < len && udp[p] != 0) {
        if ((udp[p] & 0xC0) == 0xC0) { p += 2; break; }
        p += (uint32_t)udp[p] + 1;
    }
    if (p + 5 > len) return;
    p += 5; /* NUL + QTYPE + QCLASS */
    for (uint16_t i = 0; i < an && p + 12 <= len; i++) {
        if ((udp[p] & 0xC0) == 0xC0) p += 2;
        else {
            while (p < len && udp[p]) p += (uint32_t)udp[p] + 1;
            p++;
        }
        if (p + 10 > len) return;
        uint16_t type = rd16be(udp + p), cls = rd16be(udp + p + 2);
        uint16_t rdlen = rd16be(udp + p + 8);
        p += 10;
        if (p + rdlen > len) return;
        if (type == 1 && cls == 1 && rdlen == 4) {
            memcpy(dns_answer, udp + p, 4);
            dns_done = 1;
            return;
        }
        p += rdlen;
    }
    dns_failed = 1;
}

/* In TLS mode the advertised window is the free space in the receive ring, so the
 * peer never sends more than net_tls_read() has room for. */
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

typedef struct {
    uint8_t ip[4], server[4], netmask[4], router[4], dns[4];
    uint32_t lease;
    int has_netmask, has_router, has_dns;
} dhcp_lease_t;

static volatile int dhcp_active;        /* accept packets not yet addressed to us */
static volatile int dhcp_want;          /* DHCP_OFFER or DHCP_ACK */
static volatile int dhcp_result;        /* message type received, 0 if none */
static uint32_t dhcp_xid;
static dhcp_lease_t dhcp_reply;
static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Runs in IRQ context: parse a server reply into dhcp_reply. */
static void dhcp_rx(const uint8_t *p, uint16_t len) {
    if (!dhcp_active || dhcp_result || len < 240) return;
    if (p[0] != 2 || rd32be(p + 4) != dhcp_xid || memcmp(p + 28, cfg.mac, 6) != 0) return;
    if (rd32be(p + 236) != 0x63825363u) return;

    dhcp_lease_t r;
    memset(&r, 0, sizeof(r));
    memcpy(r.ip, p + 16, 4);                       /* yiaddr */
    int type = 0;
    uint32_t i = 240;
    while (i < len && p[i] != 255) {
        uint8_t opt = p[i];
        if (opt == 0) { i++; continue; }           /* pad */
        if (i + 1 >= len) break;
        uint8_t olen = p[i + 1];
        const uint8_t *v = p + i + 2;
        if (i + 2 + olen > len) break;
        if (opt == 53 && olen >= 1) type = v[0];
        else if (opt == 1 && olen >= 4) { memcpy(r.netmask, v, 4); r.has_netmask = 1; }
        else if (opt == 3 && olen >= 4) { memcpy(r.router, v, 4); r.has_router = 1; }
        else if (opt == 6 && olen >= 4) { memcpy(r.dns, v, 4); r.has_dns = 1; }
        else if (opt == 51 && olen >= 4) r.lease = rd32be(v);
        else if (opt == 54 && olen >= 4) memcpy(r.server, v, 4);
        i += 2u + olen;
    }
    if (type == DHCP_NAK && dhcp_want == DHCP_ACK) { dhcp_result = DHCP_NAK; return; }
    if (type != dhcp_want) return;
    dhcp_reply = r;
    dhcp_result = type;
}

static void handle_udp(const uint8_t *ip, uint16_t ihl, uint16_t total) {
    const uint8_t *u = ip + ihl;
    uint16_t len = (uint16_t)(total - ihl);
    if (len < 8) return;
    uint16_t sport = rd16be(u), dport = rd16be(u + 2), ulen = rd16be(u + 4);
    if (ulen < 8 || ulen > len) return;
    if (dport == dns_port && sport == DNS_PORT && ip_eq(ip + 12, cfg.dns)) dns_rx(u + 8, (uint16_t)(ulen - 8));
    else if (dport == DHCP_CLIENT_PORT && sport == DHCP_SERVER_PORT) dhcp_rx(u + 8, (uint16_t)(ulen - 8));
}

static void handle_ipv4_extended(const uint8_t *frame, const uint8_t *ip, uint16_t ihl, uint16_t total) {
    if (ip[9] == IP_PROTO_UDP) handle_udp(ip, ihl, total);
    else if (ip[9] == IP_PROTO_TCP) tcp_input(frame, ip, ihl, total);
}

/* Keep the original receive handler's ICMP behavior, then dispatch TCP/UDP. */
static void net_rx_download_hook(const uint8_t *frame, uint16_t len) {
    if (len < 14) return;
    uint16_t type = rd16be(frame + 12);
    if (type == ETHERTYPE_ARP) { handle_arp(frame, len); return; }
    if (type != ETHERTYPE_IPV4 || len < 34) return;
    const uint8_t *ip = frame + 14;
    uint16_t ihl = (uint16_t)((ip[0] & 0x0F) * 4), total = rd16be(ip + 2);
    if ((ip[0] >> 4) != 4 || ihl < 20 || total < ihl || total > len - 14 || inet_checksum(ip, ihl) != 0) return;
    /* Packets not addressed to us (broadcasts, or DHCP replies before we have an
     * address) only go to UDP, where the DHCP client picks them up. */
    static const uint8_t limited_bcast[4] = {255, 255, 255, 255};
    if (!ip_eq(ip + 16, cfg.ip)) {
        if (ip[9] == IP_PROTO_UDP && (dhcp_active || ip_eq(ip + 16, limited_bcast))) handle_udp(ip, ihl, total);
        return;
    }
    if (ip[9] == IP_PROTO_ICMP) handle_icmp(frame, ip, ihl, total);
    else handle_ipv4_extended(frame, ip, ihl, total);
}


static int dns_lookup(const char *host, uint8_t out[4]) {
    if (net_parse_ip(host, out) == 0) return 0;
    uint8_t mac[6];
    if (resolve_next_hop(cfg.dns, mac) != 0) return -1;
    uint8_t q[256]; uint32_t n=0;
    dns_id = (uint16_t)(0x4D53 ^ get_tick());
    dns_port = (uint16_t)(40000 + (get_tick() % 20000));
    wr16be(q+n,dns_id); n+=2; wr16be(q+n,0x0100); n+=2; wr16be(q+n,1); n+=2; wr16be(q+n,0); n+=2; wr16be(q+n,0); n+=2; wr16be(q+n,0); n+=2;
    const char *p=host;
    while (*p) { const char *dot=p; while (*dot && *dot!='.') dot++; uint8_t l=(uint8_t)(dot-p); if (!l || l>63 || n+l+1+4>sizeof(q)) return -1; q[n++]=l; memcpy(q+n,p,l); n+=l; p=*dot?dot+1:dot; }
    q[n++]=0; wr16be(q+n,1); n+=2; wr16be(q+n,1); n+=2;
    dns_done=0; dns_failed=0;
    uint8_t f[14+20+8+256]; udp_build(f,mac,cfg.dns,dns_port,DNS_PORT,q,(uint16_t)n);
    if (nic_send(f,(uint16_t)(42+n))!=0) return -1;
    uint32_t start=get_tick(); while (!dns_done && !dns_failed && (uint32_t)(get_tick()-start)<300) CPU_WAIT();
    if (!dns_done) return -1;
    memcpy(out,dns_answer,4);
    return 0;
}


static int dhcp_send(int type, const dhcp_lease_t *offer) {
    static const uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static const uint8_t bcast_ip[4] = {255, 255, 255, 255};
    uint8_t m[300];
    memset(m, 0, sizeof(m));
    m[0] = 1; m[1] = 1; m[2] = 6;                  /* BOOTREQUEST, Ethernet, MAC length */
    wr32be(m + 4, dhcp_xid);
    wr16be(m + 10, 0x8000);                        /* ask the server to broadcast replies */
    memcpy(m + 28, cfg.mac, 6);
    wr32be(m + 236, 0x63825363u);                  /* magic cookie */

    uint32_t n = 240;
    m[n++] = 53; m[n++] = 1; m[n++] = (uint8_t)type;
    m[n++] = 61; m[n++] = 7; m[n++] = 1; memcpy(m + n, cfg.mac, 6); n += 6;
    static const char host[] = "terminalos";
    m[n++] = 12; m[n++] = (uint8_t)(sizeof(host) - 1); memcpy(m + n, host, sizeof(host) - 1); n += sizeof(host) - 1;
    if (type == DHCP_REQUEST) {
        m[n++] = 50; m[n++] = 4; memcpy(m + n, offer->ip, 4); n += 4;
        m[n++] = 54; m[n++] = 4; memcpy(m + n, offer->server, 4); n += 4;
    }
    m[n++] = 55; m[n++] = 4; m[n++] = 1; m[n++] = 3; m[n++] = 6; m[n++] = 51;
    m[n++] = 255;

    uint8_t f[14 + 20 + 8 + sizeof(m)];
    udp_build(f, bcast_mac, bcast_ip, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, m, sizeof(m));
    return nic_send(f, (uint16_t)sizeof(f));
}

/* Send `type` and wait up to `ticks` for the reply type set in dhcp_want. */
static int dhcp_exchange(int type, int want, const dhcp_lease_t *offer, uint32_t ticks) {
    dhcp_result = 0;
    dhcp_want = want;
    if (dhcp_send(type, offer) != 0) return 0;
    uint32_t start = get_tick();
    while (!dhcp_result && (uint32_t)(get_tick() - start) < ticks) CPU_WAIT();
    return dhcp_result;
}

static void set_static_config(void) {
    static const uint8_t ip[4]      = {10, 0, 2, 15};
    static const uint8_t netmask[4] = {255, 255, 255, 0};
    static const uint8_t gateway[4] = {10, 0, 2, 2};
    static const uint8_t dns[4]     = {10, 0, 2, 3};
    memcpy(cfg.ip, ip, 4);
    memcpy(cfg.netmask, netmask, 4);
    memcpy(cfg.gateway, gateway, 4);
    memcpy(cfg.dns, dns, 4);
    cfg_from_dhcp = 0;
    cfg_lease_seconds = 0;
}

int net_configure(void) {
    if (!net_up) { print_str("No network interface\n"); return -1; }
    print_str("[NET] DHCP: requesting an address...\n");

    uint8_t old_ip[4];
    memcpy(old_ip, cfg.ip, 4);
    memset(cfg.ip, 0, 4);                          /* DHCP requests come from 0.0.0.0 */
    dhcp_active = 1;

    int ok = 0;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        dhcp_xid = (uint32_t)READ_TSC() ^ ((uint32_t)cfg.mac[5] << 24) ^ get_tick();
        if (dhcp_exchange(DHCP_DISCOVER, DHCP_OFFER, 0, 2 * TIMER_FREQ) != DHCP_OFFER) continue;
        dhcp_lease_t offer = dhcp_reply;
        int r = dhcp_exchange(DHCP_REQUEST, DHCP_ACK, &offer, 2 * TIMER_FREQ);
        if (r == DHCP_ACK) ok = 1;
    }
    dhcp_active = 0;

    if (!ok) {
        memcpy(cfg.ip, old_ip, 4);
        char ipbuf[16];
        net_fmt_ip(ipbuf, cfg.ip);
        kprintf("[NET] DHCP: no answer, keeping %s\n", ipbuf);
        return -1;
    }

    const dhcp_lease_t *l = &dhcp_reply;
    static const uint8_t class_c[4] = {255, 255, 255, 0};
    memcpy(cfg.ip, l->ip, 4);
    memcpy(cfg.netmask, l->has_netmask ? l->netmask : class_c, 4);
    memcpy(cfg.gateway, l->has_router ? l->router : l->server, 4);
    memcpy(cfg.dns, l->has_dns ? l->dns : cfg.gateway, 4);
    cfg_from_dhcp = 1;
    cfg_lease_seconds = l->lease;

    char ipbuf[16], gwbuf[16], dnsbuf[16];
    net_fmt_ip(ipbuf, cfg.ip);
    net_fmt_ip(gwbuf, cfg.gateway);
    net_fmt_ip(dnsbuf, cfg.dns);
    kprintf("[NET] DHCP: %s, gateway %s, DNS %s\n", ipbuf, gwbuf, dnsbuf);
    return 0;
}

/* ---- For net/tcp.c (net/net_internal.h) ------------------------------------- */

const uint8_t *net_my_ip(void) { return cfg.ip; }

int net_next_hop_mac(const uint8_t dst[4], uint8_t mac[6]) { return resolve_next_hop(dst, mac); }

int net_send_ip(const uint8_t mac[6], const uint8_t dst[4], uint8_t proto, const uint8_t *payload, uint16_t len) {
    static uint8_t f[NIC_MAX_FRAME];
    if (len > NIC_MAX_FRAME - 34) return -1;
    uint64_t fl = irq_save();                    /* the receive path sends too (ACKs) */
    ip_build(f, mac, dst, proto, len);
    memcpy(f + 34, payload, len);
    int r = nic_send(f, (uint16_t)(34 + len));
    irq_restore(fl);
    return r;
}

uint16_t net_l4_checksum(const uint8_t src[4], const uint8_t dst[4], uint8_t proto, const uint8_t *seg, uint16_t len) {
    return pseudo_checksum(src, dst, proto, seg, len);
}

void net_wait(void) { CPU_WAIT(); }
uint64_t net_irq_save(void) { return irq_save(); }
void net_irq_restore(uint64_t f) { irq_restore(f); }

int net_resolve(const char *host, uint8_t out[4]) {
    return net_up ? dns_lookup(host, out) : -1;
}

/* ---- HTTPS: BearSSL reads and writes through one TCP socket ------------------- */

static int tls_socket = -1;

int net_tls_read(unsigned char *buf, size_t len) {
    if (tls_socket < 0 || !len) return -1;
    int r = tcp_recv(tls_socket, buf, (uint32_t)len, 10000);
    return r > 0 ? r : -1;
}

int net_tls_write(const unsigned char *buf, size_t len) {
    if (tls_socket < 0 || !len) return -1;
    int r = tcp_send(tls_socket, buf, (uint32_t)len);
    return r == (int)len ? r : -1;
}

/* ---- Downloads ------------------------------------------------------------------- */

static const char *tcp_error_text(int e) {
    switch (e) {
        case TCP_ERR_TIMEOUT: return "timed out";
        case TCP_ERR_REFUSED: return "connection refused";
        case TCP_ERR_RESET:   return "connection reset";
        case TCP_ERR_NOROUTE: return "no route (the gateway does not answer ARP)";
        case TCP_ERR_INTR:    return "interrupted";
        case TCP_ERR_NOSOCK:  return "no free socket";
        default:              return "failed";
    }
}

/* "scheme://host[:port]/path" -> host, port, path. */
static int parse_url(const char *url, const char *scheme, uint16_t def_port, char *host, uint32_t hostsz,
                     uint16_t *port, char *path, uint32_t pathsz) {
    size_t sl = strlen(scheme);
    if (!url || strncmp(url, scheme, sl) != 0) return -1;
    const char *p = url + sl;
    uint32_t n = 0;
    while (p[n] && p[n] != '/' && p[n] != ':' && n < hostsz - 1) n++;
    if (n == 0 || (p[n] && p[n] != '/' && p[n] != ':')) return -1;
    memcpy(host, p, n);
    host[n] = 0;
    p += n;
    *port = def_port;
    if (*p == ':') {
        p++;
        uint32_t v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p - '0'); p++; digits++; if (v > 65535) return -1; }
        if (!digits || v == 0 || (*p && *p != '/')) return -1;
        *port = (uint16_t)v;
    }
    if (!*p) { path[0] = '/'; path[1] = 0; return 0; }
    n = (uint32_t)strlen(p);
    if (n >= pathsz) return -1;
    memcpy(path, p, n + 1);
    return 0;
}

static void derive_name(const char *path, char *out, uint32_t size) {
    const char *b = path;
    for (const char *q = path; *q; q++) if (*q == '/') b = q + 1;
    if (!*b) b = "index.htm";
    uint32_t n = 0;
    while (b[n] && b[n] != '?' && n < size - 1) n++;
    memcpy(out, b, n);
    out[n] = 0;
}

static int save_download(const char *name, const uint8_t *body, uint32_t len) {
    int wr = fat32_write_file(name, body, len);         /* creates or replaces */
    if (wr < 0) { kprintf("download: cannot write %s (error %d)\n", name, wr); return -1; }
    kprintf("Downloaded %u bytes -> %s\n", len, name);
    return 0;
}

/* GET over an open socket: status, then the body into `body`. */
static int http_get(int s, const char *host, const char *path, uint8_t *body, uint32_t max, uint32_t *body_len,
                    int *status) {
    char req[600];
    int rn = k_snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n"
                                          "User-Agent: TerminalOS/1.0\r\n\r\n", path, host);
    if (tcp_send(s, req, (uint32_t)rn) != rn) return -1;
    static char head[4096];
    uint32_t hl = 0, got = 0, clen = 0;
    int have_len = 0, headers = 0, overflow = 0;
    *status = 0;
    uint8_t buf[2048];
    uint32_t last = get_tick();
    for (;;) {
        int r = tcp_recv(s, buf, sizeof(buf), 15000);
        if (r == 0) break;                               /* the server closed: done */
        if (r < 0) return r;
        uint32_t off = 0;
        if (!headers) {
            uint32_t take = (uint32_t)r < sizeof(head) - 1 - hl ? (uint32_t)r : sizeof(head) - 1 - hl;
            memcpy(head + hl, buf, take);
            hl += take;
            head[hl] = 0;
            char *sep = 0;
            for (uint32_t i = 3; i < hl && !sep; i++)
                if (head[i - 3] == '\r' && head[i - 2] == '\n' && head[i - 1] == '\r' && head[i] == '\n') sep = head + i + 1;
            if (!sep) {
                if (hl >= sizeof(head) - 1) return -1;          /* headers too long */
                continue;
            }
            headers = 1;
            if (hl >= 12 && !strncmp(head, "HTTP/", 5))
                *status = (head[9] - '0') * 100 + (head[10] - '0') * 10 + (head[11] - '0');
            have_len = parse_content_length(head, (uint32_t)(sep - head), &clen);
            uint32_t header_bytes_in_buf = (uint32_t)(sep - head) - (hl - take);
            off = header_bytes_in_buf;
        }
        uint32_t n = (uint32_t)r - off;
        if (got + n > max) { n = max - got; overflow = 1; }
        memcpy(body + got, buf + off, n);
        got += n;
        if (overflow) break;
        if ((uint32_t)(get_tick() - last) >= 5) {
            net_download_progress(got, have_len ? clen : 0);
            last = get_tick();
        }
        if (have_len && got >= clen) break;
    }
    net_download_progress(got, have_len ? clen : 0);
    *body_len = got;
    return overflow ? -4 : headers ? 0 : -1;
}

int net_download_http(const char *url, const char *filename) {
    char host[128], path[256], name[64];
    uint16_t port;
    if (!net_up) { print_str("No network interface\n"); return -1; }
    if (parse_url(url, "http://", 80, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        print_str("Usage: download http://host[:port]/path [file]\n");
        return -1;
    }
    uint8_t ip[4];
    if (dns_lookup(host, ip) != 0) { kprintf("download: DNS lookup failed for %s\n", host); return -1; }
    char iptxt[16];
    net_fmt_ip(iptxt, ip);
    kprintf("Connecting to %s (%s):%u\n", host, iptxt, (uint32_t)port);
    if (!filename) { derive_name(path, name, sizeof(name)); filename = name; }
    set_download_max();
    uint8_t *body = kmalloc(DOWNLOAD_MAX);
    if (!body) { print_str("download: out of memory\n"); return -1; }
    int s = tcp_connect(ip, port, 5000);
    if (s < 0) { kprintf("download: %s\n", tcp_error_text(s)); kfree(body); return -1; }
    download_progress_active = 0;
    net_download_progress(0, 0);
    uint32_t len = 0;
    int status = 0, r = http_get(s, host, path, body, DOWNLOAD_MAX, &len, &status);
    tcp_close(s);
    finish_download_progress();
    if (r == -4) { print_str("download: file too large\n"); kfree(body); return -1; }
    if (r < 0) { kprintf("download: %s\n", r == -1 ? "bad HTTP response" : tcp_error_text(r)); kfree(body); return -1; }
    if (status != 200) { kprintf("download: HTTP status %d\n", status); kfree(body); return -1; }
    r = save_download(filename, body, len);
    kfree(body);
    return r;
}

int net_download_https(const char *url, const char *filename) {
    char host[128], path[256], name[64];
    uint16_t port;
    if (!net_up) { print_str("No network interface\n"); return -1; }
    if (parse_url(url, "https://", 443, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        print_str("Usage: download https://host[:port]/path [file]\n");
        return -1;
    }
    uint8_t ip[4];
    if (dns_lookup(host, ip) != 0) { kprintf("download: DNS lookup failed for %s\n", host); return -1; }
    char iptxt[16];
    net_fmt_ip(iptxt, ip);
    kprintf("Connecting securely to %s (%s):%u\n", host, iptxt, (uint32_t)port);
    if (!filename) { derive_name(path, name, sizeof(name)); filename = name; }
    set_download_max();
    uint8_t *body = kmalloc(DOWNLOAD_MAX);
    if (!body) { print_str("download: out of memory\n"); return -1; }
    int s = tcp_connect(ip, port, 5000);
    if (s < 0) { kprintf("download: %s\n", tcp_error_text(s)); kfree(body); return -1; }
    tls_socket = s;
    download_progress_active = 0;
    net_download_progress(0, 0);
    uint32_t len = 0;
    int rc = tls_https_download(host, path, body, DOWNLOAD_MAX, &len);
    finish_download_progress();
    tls_socket = -1;
    tcp_close(s);
    if (rc == -5 || rc == -6) { kprintf("download: HTTP status %d\n", tls_last_http_status()); kfree(body); return -1; }
    if (rc == -4) { print_str("download: file too large\n"); kfree(body); return -1; }
    if (rc != 0) { kprintf("download: TLS/HTTPS failed (%d, BearSSL error %d)\n", rc, tls_last_error()); kfree(body); return -1; }
    int r = save_download(filename, body, len);
    kfree(body);
    return r;
}
