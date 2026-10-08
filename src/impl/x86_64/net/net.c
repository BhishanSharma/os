#include "net/net.h"
#include "drivers/rtl8139.h"
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
#define CPU_WAIT()  __asm__ volatile("hlt")
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
    rtl8139_send(f, sizeof(f));
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
static uint8_t  echo_reply_buf[RTL8139_MAX_FRAME];
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
        rtl8139_send(f, sizeof(f));
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
        if (14 + 20 + icmp_len > RTL8139_MAX_FRAME) return;
        uint8_t *f = echo_reply_buf;
        ip_build(f, frame + 6, ip + 12, IP_PROTO_ICMP, icmp_len);
        uint8_t *r = f + 14 + 20;
        memcpy(r, icmp, icmp_len);
        r[0] = ICMP_ECHO_REPLY;
        wr16be(r + 2, 0);
        wr16be(r + 2, inet_checksum(r, icmp_len));
        rtl8139_send(f, (uint16_t)(14 + 20 + icmp_len));
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
    if (!rtl8139_is_up()) return;

    rtl8139_get_mac(cfg.mac);
    static const uint8_t ip[4]      = {10, 0, 2, 15};
    static const uint8_t netmask[4] = {255, 255, 255, 0};
    static const uint8_t gateway[4] = {10, 0, 2, 2};
    static const uint8_t dns[4]     = {10, 0, 2, 3};
    memcpy(cfg.ip, ip, 4);
    memcpy(cfg.netmask, netmask, 4);
    memcpy(cfg.gateway, gateway, 4);
    memcpy(cfg.dns, dns, 4);

    rtl8139_set_rx_handler(net_rx_download_hook);
    net_up = 1;

    char m[18];
    net_fmt_mac(m, cfg.mac);
    kprintf("[NET] interface up, MAC %s\n", m);
}

int net_is_up(void) {
    return net_up;
}

const net_config_t *net_get_config(void) {
    return &cfg;
}

void net_set_debug(int on) {
    debug = on;
}

void net_print_ifconfig(void) {
    if (!net_up) {
        print_str("No network interface (is the NIC attached? QEMU needs -device rtl8139)\n");
        return;
    }

    char buf[18];
    print_str("eth0 (RTL8139)\n");
    net_fmt_mac(buf, cfg.mac);       kprintf("  MAC      %s\n", buf);
    net_fmt_ip(buf, cfg.ip);         kprintf("  IP       %s  (static)\n", buf);
    net_fmt_ip(buf, cfg.netmask);    kprintf("  Netmask  %s\n", buf);
    net_fmt_ip(buf, cfg.gateway);    kprintf("  Gateway  %s\n", buf);
    net_fmt_ip(buf, cfg.dns);        kprintf("  DNS      %s\n", buf);

    rtl8139_stats_t st;
    rtl8139_get_stats(&st);
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

    if (rtl8139_send(f, sizeof(f)) != 0) {
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
        print_str("No network interface (is the NIC attached? QEMU needs -device rtl8139)\n");
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

            if (rtl8139_send(f, sizeof(f)) != 0) {
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
#define DOWNLOAD_MAX (512u * 1024u)
#define TCP_SYN_SENT 1
#define TCP_ESTABLISHED 2
#define TCP_FIN_WAIT 3
#define TCP_CLOSED 4

static volatile int dns_done;
static volatile int dns_failed;
static uint16_t dns_id;
static uint16_t dns_port;
static uint8_t dns_answer[4];

static volatile int tcp_state;
static uint8_t tcp_peer[4];
static uint16_t tcp_peer_port;
static uint16_t tcp_local_port;
static uint32_t tcp_local_seq;
static volatile uint32_t tcp_recv_next;
static volatile uint32_t tcp_send_next;
static volatile int tcp_peer_fin;
static uint8_t *tcp_body;
static volatile uint32_t tcp_body_len;
static volatile int tcp_body_overflow;
static volatile int tcp_http_status;
static char tcp_header[4096];
static volatile uint32_t tcp_header_len;
static volatile int tcp_headers_done;
static volatile int tcp_http_bad;

/* Raw TCP transport used by the TLS adapter. */
static volatile int tcp_tls_mode;
static uint8_t *tcp_tls_rx;
static uint32_t tcp_tls_rx_size;
static volatile uint32_t tcp_tls_rx_head;
static volatile uint32_t tcp_tls_rx_tail;
static volatile int tcp_tls_overflow;
static uint8_t tcp_tls_mac[6];

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

static void tcp_send_segment(const uint8_t dst_mac[6], uint8_t flags,
                             uint32_t seq, uint32_t ack,
                             const uint8_t *payload, uint16_t plen) {
    uint8_t f[14 + 20 + 20 + TCP_MSS];
    if (plen > TCP_MSS) plen = TCP_MSS;
    ip_build(f, dst_mac, tcp_peer, IP_PROTO_TCP, (uint16_t)(20 + plen));
    uint8_t *t = f + 34;
    wr16be(t, tcp_local_port); wr16be(t + 2, tcp_peer_port);
    wr32be(t + 4, seq); wr32be(t + 8, ack);
    t[12] = 0x50; t[13] = flags;
    wr16be(t + 14, 4096); wr16be(t + 16, 0); wr16be(t + 18, 0);
    if (plen) memcpy(t + 20, payload, plen);
    wr16be(t + 16, pseudo_checksum(cfg.ip, tcp_peer, IP_PROTO_TCP, t, (uint16_t)(20 + plen)));
    rtl8139_send(f, (uint16_t)(54 + plen));
}

static void tcp_rx(const uint8_t *frame, const uint8_t *ip, uint16_t total) {
    const uint8_t *src_mac = frame + 6;
    uint16_t ihl = (uint16_t)((ip[0] & 0x0F) * 4);
    if (total < ihl + 20) return;
    const uint8_t *t = ip + ihl;
    uint16_t tlen = (uint16_t)(total - ihl);
    uint16_t sport = rd16be(t), dport = rd16be(t + 2);
    uint32_t seq = ((uint32_t)rd16be(t + 4) << 16) | rd16be(t + 6);
    uint32_t ack = ((uint32_t)rd16be(t + 8) << 16) | rd16be(t + 10);
    uint8_t off = (uint8_t)((t[12] >> 4) * 4), flags = t[13];
    if (off < 20 || off > tlen || sport != tcp_peer_port || dport != tcp_local_port || !ip_eq(ip + 12, tcp_peer)) return;
    uint16_t plen = (uint16_t)(tlen - off);
    const uint8_t *payload = t + off;

    if (tcp_state == TCP_SYN_SENT && (flags & 0x12) == 0x12 && ack == tcp_send_next) {
        tcp_recv_next = seq + 1;
        /* tcp_send_next already points past our SYN. */
        tcp_state = TCP_ESTABLISHED;
        tcp_send_segment(src_mac, 0x10, tcp_send_next, tcp_recv_next, 0, 0);
        return;
    }
    if (tcp_state != TCP_ESTABLISHED && tcp_state != TCP_FIN_WAIT) return;

    if (plen && seq == tcp_recv_next) {
        if (tcp_tls_mode) {
            if (tcp_tls_rx && tcp_tls_rx_size) {
                uint32_t used = tcp_tls_rx_head - tcp_tls_rx_tail;
                if (plen > tcp_tls_rx_size - used) {
                    tcp_tls_overflow = 1;
                } else {
                    for (uint32_t i = 0; i < plen; ++i)
                        tcp_tls_rx[(tcp_tls_rx_head + i) % tcp_tls_rx_size] = payload[i];
                    tcp_tls_rx_head += plen;
                }
            }
            tcp_recv_next += plen;
            tcp_send_segment(src_mac, 0x10, tcp_send_next, tcp_recv_next, 0, 0);
        } else if (!tcp_headers_done) {
            uint32_t old_header_len = tcp_header_len;
            uint32_t copy = plen;
            if (tcp_header_len + copy > sizeof(tcp_header) - 1) copy = sizeof(tcp_header) - 1 - tcp_header_len;
            memcpy(tcp_header + tcp_header_len, payload, copy);
            tcp_header_len += copy;
            tcp_header[tcp_header_len] = 0;
            char *sep = 0;
            for (uint32_t i = 3; i < tcp_header_len; i++)
                if (tcp_header[i-3]=='\r' && tcp_header[i-2]=='\n' && tcp_header[i-1]=='\r' && tcp_header[i]=='\n') { sep = tcp_header + i + 1; break; }
            if (sep) {
                tcp_headers_done = 1;
                if (tcp_header_len >= 12 && tcp_header[0]=='H' && tcp_header[1]=='T' && tcp_header[2]=='T' && tcp_header[3]=='P' && tcp_header[4]=='/')
                    tcp_http_status = (tcp_header[9]>='0'&&tcp_header[9]<='9'&&tcp_header[10]>='0'&&tcp_header[10]<='9'&&tcp_header[11]>='0'&&tcp_header[11]<='9') ? (tcp_header[9]-'0')*100+(tcp_header[10]-'0')*10+(tcp_header[11]-'0') : 0;
                uint32_t body_off = (uint32_t)(sep - tcp_header);
                uint32_t body_n = tcp_header_len > body_off ? tcp_header_len - body_off : 0;
                if (tcp_body && body_n) {
                    if (body_n > DOWNLOAD_MAX) body_n = DOWNLOAD_MAX;
                    memcpy(tcp_body, sep, body_n); tcp_body_len = body_n;
                }
                /* If the header terminator was found in this packet, body_n already
                 * contains every body byte that fit in tcp_header. Only append bytes
                 * that were beyond the portion copied into tcp_header. */
                if (copy < plen) {
                    uint32_t extra = plen - copy;
                    if (tcp_body_len + extra > DOWNLOAD_MAX) { extra = DOWNLOAD_MAX - tcp_body_len; tcp_body_overflow = 1; }
                    memcpy(tcp_body + tcp_body_len, payload + copy, extra); tcp_body_len += extra;
                }
            }
        } else if (tcp_body) {
            uint32_t n = plen;
            if (tcp_body_len + n > DOWNLOAD_MAX) { n = DOWNLOAD_MAX - tcp_body_len; tcp_body_overflow = 1; }
            memcpy(tcp_body + tcp_body_len, payload, n); tcp_body_len += n;
        }
        tcp_recv_next += plen;
        tcp_send_segment(src_mac, 0x10, tcp_send_next, tcp_recv_next, 0, 0);
    }
    if (flags & 0x01) {
        if (seq + plen == tcp_recv_next) tcp_recv_next++;
        tcp_send_segment(src_mac, 0x10, tcp_send_next, tcp_recv_next, 0, 0);
        tcp_peer_fin = 1;
        tcp_state = TCP_CLOSED;
    }
    (void)ack;
}

static void handle_udp(const uint8_t *ip, uint16_t ihl, uint16_t total) {
    const uint8_t *u = ip + ihl;
    uint16_t len = (uint16_t)(total - ihl);
    if (len < 8) return;
    uint16_t sport = rd16be(u), dport = rd16be(u + 2), ulen = rd16be(u + 4);
    if (ulen < 8 || ulen > len) return;
    if (dport == dns_port && sport == DNS_PORT && ip_eq(ip + 12, cfg.dns)) dns_rx(u + 8, (uint16_t)(ulen - 8));
}

static void handle_ipv4_extended(const uint8_t *frame, const uint8_t *ip, uint16_t ihl, uint16_t total) {
    if (ip[9] == IP_PROTO_UDP) handle_udp(ip, ihl, total);
    else if (ip[9] == IP_PROTO_TCP) tcp_rx(frame, ip, total);
}

/* Keep the original receive handler's ICMP behavior, then dispatch TCP/UDP. */
static void net_rx_download_hook(const uint8_t *frame, uint16_t len) {
    if (len < 14) return;
    uint16_t type = rd16be(frame + 12);
    if (type == ETHERTYPE_ARP) { handle_arp(frame, len); return; }
    if (type != ETHERTYPE_IPV4 || len < 34) return;
    const uint8_t *ip = frame + 14;
    uint16_t ihl = (uint16_t)((ip[0] & 0x0F) * 4), total = rd16be(ip + 2);
    if ((ip[0] >> 4) != 4 || ihl < 20 || total < ihl || total > len - 14 || inet_checksum(ip, ihl) != 0 || !ip_eq(ip + 16, cfg.ip)) return;
    if (ip[9] == IP_PROTO_ICMP) handle_icmp(frame, ip, ihl, total);
    else handle_ipv4_extended(frame, ip, ihl, total);
}

static int parse_http_url(const char *url, char *host, uint32_t hostsz, uint16_t *port, char *path, uint32_t pathsz) {
    if (!url || strncmp(url, "http://", 7) != 0) return -1;
    const char *p = url + 7, *slash = 0;
    uint32_t i = 0;
    while (p[i] && p[i] != '/' && p[i] != ':' && i < hostsz - 1) i++;
    if (i == 0) return -1;
    memcpy(host, p, i); host[i] = 0; p += i; *port = 80;
    if (*p == ':') {
        p++; uint32_t v = 0; int digits = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; digits++; if (v > 65535) return -1; }
        if (!digits || v == 0 || *p != '/') return -1;
        *port = (uint16_t)v;
    }
    slash = p;
    if (!*slash) { path[0]='/'; path[1]=0; return 0; }
    if (*slash != '/') return -1;
    uint32_t n = 0; while (slash[n] && n < pathsz - 1) n++;
    if (slash[n]) return -1;
    memcpy(path, slash, n); path[n]=0; return 0;
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
    if (rtl8139_send(f,(uint16_t)(42+n))!=0) return -1;
    uint32_t start=get_tick(); while (!dns_done && !dns_failed && (uint32_t)(get_tick()-start)<300) CPU_WAIT();
    if (!dns_done) return -1;
    memcpy(out,dns_answer,4);
    return 0;
}


int net_tls_read(unsigned char *buf, size_t len) {
    if (!tcp_tls_mode || !tcp_tls_rx || tcp_tls_rx_size == 0 || len == 0 || tcp_tls_overflow) return -1;
    uint32_t start = get_tick();
    for (;;) {
        uint32_t avail = tcp_tls_rx_head - tcp_tls_rx_tail;
        if (avail) {
            if (len > avail) len = avail;
            for (size_t i = 0; i < len; ++i)
                buf[i] = tcp_tls_rx[(tcp_tls_rx_tail + (uint32_t)i) % tcp_tls_rx_size];
            tcp_tls_rx_tail += (uint32_t)len;
            return (int)len;
        }
        if (tcp_state == TCP_CLOSED || (uint32_t)(get_tick() - start) > 1000) return -1;
        CPU_WAIT();
    }
}

int net_tls_write(const unsigned char *buf, size_t len) {
    if (!tcp_tls_mode || !buf || len == 0) return -1;
    size_t done = 0;
    while (done < len) {
        uint16_t n = (uint16_t)((len - done > TCP_MSS) ? TCP_MSS : (len - done));
        tcp_send_segment(tcp_tls_mac, 0x18, tcp_send_next, tcp_recv_next, buf + done, n);
        tcp_send_next += n;
        done += n;
    }
    return (int)done;
}

static int net_tcp_connect(const uint8_t ip[4], uint16_t port, uint8_t mac_out[6], int tls_mode) {
    memcpy(tcp_peer, ip, 4);
    tcp_peer_port = port;
    tcp_local_port = (uint16_t)(40000 + (get_tick() % 20000));
    tcp_local_seq = 0x10000000u + get_tick();
    tcp_send_next = tcp_local_seq;
    tcp_recv_next = 0;
    tcp_state = TCP_SYN_SENT;
    tcp_tls_mode = tls_mode;
    tcp_peer_fin = 0;
    if (resolve_next_hop(ip, mac_out) != 0) return -1;
    memcpy(tcp_tls_mac, mac_out, 6);
    tcp_send_segment(mac_out, 0x02, tcp_send_next, 0, 0, 0);
    tcp_send_next++;
    uint32_t start = get_tick();
    while (tcp_state == TCP_SYN_SENT && (uint32_t)(get_tick() - start) < 1000) CPU_WAIT();
    return tcp_state == TCP_ESTABLISHED ? 0 : -1;
}

static void net_tcp_disconnect(void) {
    tcp_tls_mode = 0;
    tcp_state = TCP_CLOSED;
    if (tcp_tls_rx) { kfree(tcp_tls_rx); tcp_tls_rx = 0; }
    tcp_tls_rx_size = tcp_tls_rx_head = tcp_tls_rx_tail = 0;
    tcp_tls_overflow = 0;
}

int net_download_https(const char *url, const char *filename) {
    char host[128], path[256], derived[64];
    if (!net_up) { print_str("No network interface\n"); return -1; }
    if (!url || strncmp(url, "https://", 8) != 0) return -1;
    const char *p = url + 8;
    uint32_t hn = 0;
    while (p[hn] && p[hn] != '/' && p[hn] != ':' && hn < sizeof(host)-1) hn++;
    if (hn == 0) return -1;
    memcpy(host, p, hn); host[hn] = 0; p += hn;
    uint16_t port = 443;
    if (*p == ':') {
        p++; uint32_t v = 0; int digits = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; digits++; if (v > 65535) return -1; }
        if (!digits || v == 0 || *p != '/') return -1;
        port = (uint16_t)v;
    }
    if (!*p) { path[0] = '/'; path[1] = 0; } else {
        uint32_t n = 0; while (p[n] && n < sizeof(path)-1) n++;
        if (p[n]) return -1; memcpy(path, p, n); path[n] = 0;
    }

    uint8_t ip[4];
    if (dns_lookup(host, ip) != 0) { kprintf("download: DNS lookup failed for %s\n", host); return -1; }
    char iptxt[16]; net_fmt_ip(iptxt, ip);
    kprintf("Connecting securely to %s (%s):%u\n", host, iptxt, (uint32_t)port);

    const char *outname = filename;
    if (!outname) {
        const char *b = path;
        for (const char *q = path; *q; q++) if (*q == '/') b = q + 1;
        if (!*b) b = "index.htm";
        uint32_t n = 0; while (b[n] && b[n] != '?' && n < sizeof(derived)-1) n++;
        memcpy(derived, b, n); derived[n] = 0; outname = derived;
    }
    if (fat32_file_exists(outname)) fat32_delete_file(outname);
    if (fat32_create_file(outname) != 0) { kprintf("download: cannot create %s\n", outname); return -1; }

    tcp_tls_rx_size = 32768;
    tcp_tls_rx = (uint8_t*)kmalloc(tcp_tls_rx_size);
    uint8_t *body = (uint8_t*)kmalloc(DOWNLOAD_MAX);
    if (!tcp_tls_rx || !body) { kfree(tcp_tls_rx); kfree(body); print_str("download: out of memory\n"); return -1; }
    tcp_tls_rx_head = tcp_tls_rx_tail = 0;
    tcp_tls_overflow = 0;

    uint8_t mac[6];
    if (net_tcp_connect(ip, port, mac, 1) != 0) { net_tcp_disconnect(); kfree(body); print_str("download: TCP connection timeout\n"); return -1; }
    uint32_t body_len = 0;
    int rc = tls_https_download(host, path, body, DOWNLOAD_MAX, &body_len);
    net_tcp_disconnect();
    if (rc != 0) { kprintf("download: TLS/HTTPS failed (%d)\n", rc); kfree(body); return -1; }
    int wr = fat32_write_file(outname, body, body_len);
    kfree(body);
    if (wr < 0) { kprintf("download: FAT32 write failed (%d)\n", wr); return -1; }
    kprintf("Downloaded %u bytes -> %s\n", body_len, outname);
    return 0;
}

int net_download_http(const char *url, const char *filename) {
    char host[128], path[256], derived[32]; uint16_t port;
    if (!net_up) { print_str("No network interface\n"); return -1; }
    if (parse_http_url(url,host,sizeof(host),&port,path,sizeof(path)) != 0) { print_str("Usage: download http://host[:port]/path [file]\n"); return -1; }
    uint8_t ip[4]; if (dns_lookup(host,ip)!=0) { kprintf("download: DNS lookup failed for %s\n",host); return -1; }
    net_fmt_ip(derived,ip); kprintf("Connecting to %s (%s):%u\n",host,derived,(uint32_t)port);

    const char *outname=filename;
    if (!outname) {
        const char *b=path; for (const char *q=path; *q; q++) if (*q=='/') b=q+1;
        if (!*b) b="index.htm";
        uint32_t n=0; while (b[n] && b[n]!='?' && n<sizeof(derived)-1) n++;
        memcpy(derived,b,n); derived[n]=0; outname=derived;
    }
    if (fat32_file_exists(outname)) fat32_delete_file(outname);
    if (fat32_create_file(outname)!=0) { kprintf("download: cannot create %s\n",outname); return -1; }
    tcp_body=kmalloc(DOWNLOAD_MAX); if (!tcp_body) { print_str("download: out of memory\n"); return -1; }
    tcp_body_len=0; tcp_body_overflow=0; tcp_headers_done=0; tcp_header_len=0; tcp_http_status=0; tcp_peer_fin=0; tcp_http_bad=0;
    uint8_t mac[6]; if (net_tcp_connect(ip, port, mac, 0) != 0) { print_str("download: TCP connection timeout\n"); kfree(tcp_body); return -1; }
    char req[512]; uint32_t rn=0;
    const char *parts[] = {"GET ", path, " HTTP/1.0\r\nHost: ", host, "\r\nConnection: close\r\nUser-Agent: TerminalOS/1.0\r\n\r\n"};
    for (int pi=0; pi<5; pi++) { const char *z=parts[pi]; while (*z && rn<sizeof(req)-1) req[rn++]=*z++; }
    req[rn]=0;
    tcp_send_segment(mac,0x18,tcp_send_next,tcp_recv_next,(const uint8_t*)req,(uint16_t)rn); tcp_send_next += (uint32_t)rn; tcp_state=TCP_FIN_WAIT;
    uint32_t start=get_tick(); while (tcp_state!=TCP_CLOSED && (uint32_t)(get_tick()-start)<1000) CPU_WAIT();
    tcp_tls_mode = 0;
    if (!tcp_headers_done || tcp_http_status != 200 || tcp_body_overflow) { kprintf("download: HTTP status %d%s\n",tcp_http_status,tcp_body_overflow?" or file too large":""); kfree(tcp_body); return -1; }
    int write_result = fat32_write_file(outname,tcp_body,tcp_body_len);
    if (write_result < 0) { kprintf("download: FAT32 write failed (%d)\n", write_result); kfree(tcp_body); return -1; }
    kprintf("Downloaded %u bytes -> %s\n",tcp_body_len,outname); kfree(tcp_body); return 0;
}
