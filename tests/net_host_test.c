/* Host-side test for src/impl/x86_64/net/net.c.
 *
 * Builds net.c with -DNET_HOST_TEST and replaces the NIC, timer, keyboard and
 * printing with mocks. A tiny simulated gateway answers ARP and ICMP so ping can
 * be exercised without QEMU. Run with:  make test-net
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include "net/net.h"
#include "drivers/rtl8139.h"

/* ---- mocks ------------------------------------------------------------ */
static uint32_t g_tick = 0;
static uint64_t g_tsc = 1000;
static char     g_out[65536];
static size_t   g_outlen = 0;
static rtl8139_rx_cb_t g_rx = NULL;
static uint8_t  g_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static const uint8_t GW_MAC[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};

#define CYCLES_PER_US 3000ULL            /* pretend 3 GHz TSC */
#define NQ 16
static struct { uint8_t d[1600]; uint16_t len; uint32_t delay_us; } q[NQ];
static int nq = 0;

static void out(const char *s) { size_t n = strlen(s); memcpy(g_out + g_outlen, s, n); g_outlen += n; g_out[g_outlen] = 0; }
void print_str(const char *s) { out(s); }
void kprintf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); char b[64];
    for (; *fmt; fmt++) {
        if (*fmt != '%') { char c[2] = {*fmt, 0}; out(c); continue; }
        fmt++;
        switch (*fmt) {
        case 'u': snprintf(b, sizeof b, "%u", va_arg(ap, uint32_t)); out(b); break;
        case 'd': snprintf(b, sizeof b, "%d", va_arg(ap, int)); out(b); break;
        case 'x': snprintf(b, sizeof b, "%x", va_arg(ap, uint32_t)); out(b); break;
        case 's': out(va_arg(ap, char *)); break;
        case '%': out("%"); break;
        default: break;
        }
    }
    va_end(ap);
}
int rtl8139_is_up(void) { return 1; }
void rtl8139_get_mac(uint8_t m[6]) { memcpy(m, g_mac, 6); }
void rtl8139_set_rx_handler(rtl8139_rx_cb_t cb) { g_rx = cb; }
void rtl8139_get_stats(rtl8139_stats_t *s) { memset(s, 0, sizeof *s); }
uint32_t get_tick(void) { return g_tick; }
int get_char(void) { return 0; }

/* The "NIC interrupt": frames queued by the gateway are delivered on the next wait. */
void net_test_wait(void) {
    /* hlt returns at the moment the interrupt fires, so deliver first (charging the
     * wire delay), and only then let the rest of the 10 ms tick elapse. */
    int n = nq; nq = 0;
    static uint8_t copy[NQ][1600]; static uint16_t clen[NQ];
    for (int i = 0; i < n; i++) { memcpy(copy[i], q[i].d, q[i].len); clen[i] = q[i].len; g_tsc += q[i].delay_us * CYCLES_PER_US; g_rx(copy[i], clen[i]); }
    g_tick++;
    g_tsc += 10000 * CYCLES_PER_US;
}
uint64_t net_test_tsc(void) { return g_tsc; }
void sleep(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 10) net_test_wait(); }

/* ---- independent checksum (so we don't just test net.c against itself) -- */
static uint16_t cksum(const uint8_t *p, int n) {
    uint32_t s = 0;
    for (int i = 0; i + 1 < n; i += 2) s += (p[i] << 8) | p[i + 1];
    if (n & 1) s += p[n - 1] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

/* ---- simulated network ------------------------------------------------- */
enum { GW_NORMAL, GW_DROP_ICMP, GW_UNREACH, GW_BADSUM, GW_TTL_EXCEEDED };
static int gw_mode = GW_NORMAL;
static int gw_answers_arp = 1;
static uint32_t gw_delay_us = 350;
static int tx_count = 0, tx_arp_requests = 0, tx_icmp = 0;

static void enq(const uint8_t *d, int len, uint32_t delay) {
    memcpy(q[nq].d, d, len); q[nq].len = len; q[nq].delay_us = delay; nq++;
}

static void build_ip(uint8_t *f, const uint8_t *dmac, const uint8_t *smac, const uint8_t *src,
                     const uint8_t *dst, uint8_t ttl, uint8_t proto, int plen) {
    memcpy(f, dmac, 6); memcpy(f + 6, smac, 6); f[12] = 8; f[13] = 0;
    uint8_t *ip = f + 14;
    ip[0] = 0x45; ip[1] = 0; ip[2] = (20 + plen) >> 8; ip[3] = (20 + plen) & 0xff;
    ip[4] = ip[5] = 0; ip[6] = ip[7] = 0; ip[8] = ttl; ip[9] = proto; ip[10] = ip[11] = 0;
    memcpy(ip + 12, src, 4); memcpy(ip + 16, dst, 4);
    uint16_t c = cksum(ip, 20); ip[10] = c >> 8; ip[11] = c & 0xff;
}

int rtl8139_send(const void *frame, uint16_t len) {
    const uint8_t *f = frame;
    tx_count++;
    uint16_t type = (f[12] << 8) | f[13];
    if (type == 0x0806) {                                  /* ARP */
        if (f[21] == 1) {
            tx_arp_requests++;
            if (gw_answers_arp) {
                uint8_t r[42];
                memcpy(r, f + 6, 6); memcpy(r + 6, GW_MAC, 6); r[12] = 8; r[13] = 6;
                memcpy(r + 14, f + 14, 6); r[20] = 0; r[21] = 2;
                memcpy(r + 22, GW_MAC, 6); memcpy(r + 28, f + 38, 4);   /* we are whoever was asked */
                memcpy(r + 32, f + 22, 6); memcpy(r + 38, f + 28, 4);
                enq(r, 42, 50);
            }
        }
        return 0;
    }
    if (type != 0x0800) return 0;
    const uint8_t *ip = f + 14;
    if (cksum(ip, 20) != 0) { printf("TEST BUG: TX IP checksum bad\n"); exit(1); }
    static const uint8_t WHO_MAC[6] = {2,2,2,2,2,2};
    if (memcmp(f, GW_MAC, 6) != 0 && memcmp(f, WHO_MAC, 6) != 0) { printf("TEST BUG: frame not addressed to gateway MAC\n"); exit(1); }
    if (ip[9] != 1) return 0;
    const uint8_t *icmp = ip + 20;
    int ilen = ((ip[2] << 8) | ip[3]) - 20;
    if (cksum(icmp, ilen) != 0) { printf("TEST BUG: TX ICMP checksum bad\n"); exit(1); }
    if (icmp[0] != 8) return 0;
    tx_icmp++;

    uint8_t r[1600];
    if (gw_mode == GW_DROP_ICMP) return 0;
    if (gw_mode == GW_UNREACH || gw_mode == GW_TTL_EXCEEDED) {
        /* ICMP error from the gateway quoting our IP header + 8 bytes */
        int plen = 8 + 20 + 8;
        build_ip(r, f + 6, GW_MAC, (const uint8_t[]){10, 0, 2, 2}, ip + 12, 255, 1, plen);
        uint8_t *e = r + 34;
        e[0] = gw_mode == GW_UNREACH ? 3 : 11; e[1] = gw_mode == GW_UNREACH ? 1 : 0;
        e[2] = e[3] = 0; e[4] = e[5] = e[6] = e[7] = 0;
        memcpy(e + 8, ip, 20); memcpy(e + 28, icmp, 8);
        uint16_t c = cksum(e, plen); e[2] = c >> 8; e[3] = c & 0xff;
        enq(r, 14 + 20 + plen, gw_delay_us);
        return 0;
    }
    /* echo reply from the destination (ttl 117 off-link, 64 on-link) */
    build_ip(r, f + 6, GW_MAC, ip + 16, ip + 12, ip[16] == 10 ? 64 : 117, 1, ilen);
    uint8_t *e = r + 34;
    memcpy(e, icmp, ilen); e[0] = 0; e[2] = e[3] = 0;
    uint16_t c = cksum(e, ilen); e[2] = c >> 8; e[3] = c & 0xff;
    if (gw_mode == GW_BADSUM) e[2] ^= 0xff;
    enq(r, 14 + 20 + ilen, gw_delay_us);
    return 0;
}

/* ---- test framework ---------------------------------------------------- */
static int fails = 0, passes = 0;
#define CHECK(cond, msg) do { if (cond) { passes++; } else { fails++; printf("  FAIL: %s\n", msg); } } while (0)
static int contains(const char *s) { return strstr(g_out, s) != NULL; }
static void reset(void) { g_outlen = 0; g_out[0] = 0; nq = 0; tx_count = tx_arp_requests = tx_icmp = 0; }
static void show(const char *title) { printf("--- %s ---\n%s\n", title, g_out); }

int main(void) {
    uint8_t ip[4];

    printf("== net_parse_ip ==\n");
    CHECK(net_parse_ip("10.0.2.2", ip) == 0 && ip[0] == 10 && ip[3] == 2, "valid ip");
    CHECK(net_parse_ip("255.255.255.255", ip) == 0, "max ip");
    CHECK(net_parse_ip("256.1.1.1", ip) != 0, "256 rejected");
    CHECK(net_parse_ip("1.2.3", ip) != 0, "3 parts rejected");
    CHECK(net_parse_ip("1.2.3.4.5", ip) != 0, "5 parts rejected");
    CHECK(net_parse_ip("1.2.3.4x", ip) != 0, "trailing junk rejected");
    CHECK(net_parse_ip("a.b.c.d", ip) != 0, "letters rejected");
    CHECK(net_parse_ip("", ip) != 0, "empty rejected");
    CHECK(net_parse_ip("1..2.3", ip) != 0, "empty octet rejected");
    CHECK(net_parse_ip("1.2.3.1000", ip) != 0, "4 digits rejected");

    net_init();
    CHECK(g_rx != NULL, "rx handler installed");

    printf("== ping gateway (on-link, ARP then echo) ==\n");
    reset();
    net_parse_ip("10.0.2.2", ip);
    int rc = net_ping(ip, 4);
    show("ping 10.0.2.2");
    CHECK(rc == PING_OK, "returns OK");
    CHECK(tx_arp_requests == 1, "exactly one ARP request");
    CHECK(tx_icmp == 4, "4 echo requests sent");
    CHECK(contains("icmp_seq=1 ttl=64 time=0.35 ms"), "seq 1 reply with 0.35 ms");
    CHECK(contains("icmp_seq=4 "), "seq 4 reply");
    CHECK(contains("4 packets transmitted, 4 received, 0% packet loss"), "summary 0% loss");
    CHECK(contains("rtt min/avg/max = 0.35/0.35/0.35 ms"), "rtt stats");
    CHECK(contains("32 bytes from 10.0.2.2"), "32 payload bytes echoed");

    printf("== ping again: ARP cache is used ==\n");
    reset();
    rc = net_ping(ip, 1);
    CHECK(rc == PING_OK && tx_arp_requests == 0, "no new ARP request");

    printf("== ping off-link host (goes via gateway) ==\n");
    reset();
    net_parse_ip("8.8.8.8", ip);
    rc = net_ping(ip, 2);
    show("ping 8.8.8.8");
    CHECK(rc == PING_OK, "OK");
    CHECK(contains("ttl=117"), "ttl from remote host");
    CHECK(tx_arp_requests == 0, "gateway MAC already cached, no ARP for 8.8.8.8");

    printf("== timeouts ==\n");
    reset(); gw_mode = GW_DROP_ICMP;
    rc = net_ping(ip, 2);
    show("ping with no replies");
    CHECK(rc == PING_NO_REPLY, "returns NO_REPLY");
    CHECK(contains("Request timeout for icmp_seq=1"), "timeout line");
    CHECK(contains("2 packets transmitted, 0 received, 100% packet loss"), "100% loss");

    printf("== destination unreachable ==\n");
    reset(); gw_mode = GW_UNREACH;
    rc = net_ping(ip, 1);
    show("unreachable");
    CHECK(rc == PING_NO_REPLY, "no success");
    CHECK(contains("From 10.0.2.2: icmp_seq=1 Destination Host Unreachable"), "unreachable reported");

    printf("== ttl exceeded ==\n");
    reset(); gw_mode = GW_TTL_EXCEEDED;
    rc = net_ping(ip, 1);
    CHECK(contains("Time to live exceeded"), "ttl exceeded reported");

    printf("== corrupted reply is dropped ==\n");
    reset(); gw_mode = GW_BADSUM;
    rc = net_ping(ip, 1);
    CHECK(rc == PING_NO_REPLY && contains("Request timeout"), "bad ICMP checksum ignored");
    gw_mode = GW_NORMAL;

    printf("== on-link host that never answers ARP ==\n");
    reset(); gw_answers_arp = 0;
    net_parse_ip("10.0.2.77", ip);
    rc = net_ping(ip, 1);
    show("no ARP reply");
    CHECK(rc == PING_NO_ROUTE, "returns NO_ROUTE");
    CHECK(tx_arp_requests == 3, "3 ARP attempts");
    CHECK(tx_icmp == 0, "no echo sent");
    gw_answers_arp = 1;

    printf("== ping ourselves (loopback) ==\n");
    reset();
    net_parse_ip("10.0.2.15", ip);
    rc = net_ping(ip, 2);
    CHECK(rc == PING_OK && tx_count == 0, "loopback never touches the NIC");
    reset();
    net_parse_ip("127.0.0.1", ip);
    CHECK(net_ping(ip, 1) == PING_OK && tx_count == 0, "127.0.0.1 loopback");

    printf("== we answer ARP requests for our IP ==\n");
    reset();
    {
        uint8_t a[42] = {0}; const uint8_t who[6] = {2,2,2,2,2,2};
        memset(a, 0xff, 6); memcpy(a + 6, who, 6); a[12] = 8; a[13] = 6;
        a[15] = 1; a[16] = 8; a[18] = 6; a[19] = 4; a[21] = 1;
        memcpy(a + 22, who, 6); a[28] = 10; a[29] = 0; a[30] = 2; a[31] = 99;
        a[38] = 10; a[39] = 0; a[40] = 2; a[41] = 15;
        int before = tx_count;
        g_rx(a, 42);
        CHECK(tx_count == before + 1, "sent an ARP reply");
        /* and it was learned: pinging 10.0.2.99 should not need ARP */
    }
    reset();
    net_parse_ip("10.0.2.99", ip);
    gw_answers_arp = 0;
    rc = net_ping(ip, 1);
    CHECK(tx_arp_requests == 0, "requester's MAC was cached from its ARP request");
    gw_answers_arp = 1;

    printf("== we answer echo requests addressed to us ==\n");
    {
        uint8_t f[14 + 20 + 8 + 16]; uint8_t src[4] = {10,0,2,2}, me[4] = {10,0,2,15};
        build_ip(f, g_mac, GW_MAC, src, me, 64, 1, 8 + 16);
        uint8_t *e = f + 34; memset(e, 0, 24); e[0] = 8; e[4] = 0x12; e[5] = 0x34; e[7] = 9;
        for (int i = 0; i < 16; i++) e[8 + i] = 'A' + i;
        uint16_t c = cksum(e, 24); e[2] = c >> 8; e[3] = c & 0xff;
        /* capture what we transmit */
        reset(); gw_mode = GW_DROP_ICMP;     /* gateway sim must not interfere */
        int before = tx_count;
        g_rx(f, sizeof f);
        CHECK(tx_count == before + 1, "echo reply transmitted");
        gw_mode = GW_NORMAL;
    }

    printf("== malformed frames never crash ==\n");
    {
        uint8_t junk[64]; 
        for (int n = 0; n < 2000; n++) {
            for (int i = 0; i < 64; i++) junk[i] = rand();
            junk[12] = (n & 1) ? 8 : 8; junk[13] = (n & 2) ? 0 : 6;
            g_rx(junk, 14 + rand() % 50);
        }
        CHECK(1, "2000 random frames survived");
    }

    printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
