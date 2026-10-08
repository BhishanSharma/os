#include "net/net.h"
#include "drivers/rtl8139.h"
#include "drivers/timer.h"
#include "lib/print.h"
#include "lib/string.h"
#include <stdint.h>

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

/* ---- receive path (runs in IRQ context: keep it short) ---------------- */

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

    /* ARP reply (opcode 2) whose sender IP is the one the selftest asked about. */
    if (type == ETHERTYPE_ARP && len >= 42 && rd16be(frame + 20) == 2 &&
        memcmp(frame + 28, selftest_target_ip, 4) == 0) {
        memcpy(selftest_reply_mac, frame + 22, 6);
        selftest_got_reply = 1;
    }
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

    rtl8139_set_rx_handler(net_rx);
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
