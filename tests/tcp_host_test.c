/* Host-side test for src/impl/x86_64/net/tcp.c against a simulated peer.
 *
 * The "network" is a queue of segments with a delay each; a share of them is
 * dropped and the delays vary, so segments also arrive out of order. The peer
 * is a small TCP of its own (go-back-N, cumulative ACKs). Run with:
 *   gcc -I src/intf tests/tcp_host_test.c src/impl/x86_64/net/tcp.c && ./a.out
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include "net/tcp.h"
#include "net/net_internal.h"

static int failures;
#define CHECK(c, what) do { int ok_ = (c); printf("%s  %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) failures++; } while (0)

/* ---- mocks ------------------------------------------------------------------ */
static uint32_t now_tick;
uint32_t get_tick(void) { return now_tick; }
void *kmalloc(uint64_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
void kprintf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
int k_snprintf(char *b, size_t n, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(b, n, fmt, ap); va_end(ap); return r; }

static const uint8_t ME[4] = {10, 0, 0, 1}, PEER[4] = {10, 0, 0, 2};
const uint8_t *net_my_ip(void) { return ME; }
int net_next_hop_mac(const uint8_t dst[4], uint8_t mac[6]) { (void)dst; memset(mac, 0x22, 6); return 0; }
uint64_t net_irq_save(void) { return 0; }
void net_irq_restore(uint64_t f) { (void)f; }

static uint16_t csum(const uint8_t s[4], const uint8_t d[4], uint8_t proto, const uint8_t *p, uint16_t len) {
    uint32_t sum = (s[0] << 8 | s[1]) + (s[2] << 8 | s[3]) + (d[0] << 8 | d[1]) + (d[2] << 8 | d[3]) + proto + len;
    while (len > 1) { sum += p[0] << 8 | p[1]; p += 2; len -= 2; }
    if (len) sum += p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}
uint16_t net_l4_checksum(const uint8_t s[4], const uint8_t d[4], uint8_t proto, const uint8_t *seg, uint16_t len) {
    return csum(s, d, proto, seg, len);
}

/* The link: segments in flight, each way. */
typedef struct { uint8_t d[1600]; uint16_t len; uint32_t at; int to_peer; } pkt_t;
static pkt_t link_q[4096];
static int nlink;
static int drop_pct = 0, jitter = 0;
static unsigned rng = 12345;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return (rng >> 16) & 0x7FFF; }

static void link_put(const uint8_t *seg, uint16_t len, int to_peer) {
    if (drop_pct && (int)(rnd() % 100) < drop_pct) return;
    if (nlink >= 4096) return;
    pkt_t *p = &link_q[nlink++];
    memcpy(p->d, seg, len);
    p->len = len;
    p->at = now_tick + 1 + (jitter ? rnd() % (unsigned)jitter : 0);
    p->to_peer = to_peer;
}

int net_send_ip(const uint8_t mac[6], const uint8_t dst[4], uint8_t proto, const uint8_t *payload, uint16_t len) {
    (void)mac; (void)dst; (void)proto;
    link_put(payload, len, 1);
    return 0;
}

/* ---- the peer ------------------------------------------------------------------- */
static struct {
    int listening_port, closed_port;
    int state;                     /* 0 none, 1 established, 2 we sent FIN */
    uint16_t port, our_port;       /* peer's port, the OS side's port */
    uint32_t snd_una, snd_nxt, iss, rcv_nxt, wnd;
    uint8_t *out; uint32_t out_len, out_sent_base;  /* data to send: out[i] has seq iss+1+i */
    uint8_t *in; uint32_t in_len;
    int fin_rcvd, fin_send, fin_sent;
    uint32_t rto_at;
} peer;

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void peer_send(uint8_t flags, uint32_t seq, const uint8_t *data, uint16_t len, uint16_t sport, uint16_t dport) {
    uint8_t s[1600];
    memset(s, 0, 20);
    put16(s, sport); put16(s + 2, dport); put32(s + 4, seq); put32(s + 8, peer.rcv_nxt);
    s[12] = 5 << 4; s[13] = flags; put16(s + 14, 65535);
    if (len) memcpy(s + 20, data, len);
    put16(s + 16, csum(PEER, ME, 6, s, (uint16_t)(20 + len)));
    link_put(s, (uint16_t)(20 + len), 0);
}

static void peer_output(void) {
    if (peer.state != 1) return;
    while (peer.snd_nxt - (peer.iss + 1) < peer.out_len && peer.snd_nxt - peer.snd_una < peer.wnd) {
        uint32_t off = peer.snd_nxt - (peer.iss + 1), n = peer.out_len - off;
        if (n > 1000) n = 1000;
        if (n > peer.wnd - (peer.snd_nxt - peer.snd_una)) n = peer.wnd - (peer.snd_nxt - peer.snd_una);
        if (!n) break;
        peer_send(0x18, peer.snd_nxt, peer.out + off, (uint16_t)n, peer.port, peer.our_port);
        peer.snd_nxt += n;
        if (!peer.rto_at) peer.rto_at = now_tick + 30;
    }
    if (peer.fin_send && !peer.fin_sent && peer.snd_nxt - (peer.iss + 1) == peer.out_len) {
        peer_send(0x11, peer.snd_nxt, 0, 0, peer.port, peer.our_port);
        peer.snd_nxt++;
        peer.fin_sent = 1;
        if (!peer.rto_at) peer.rto_at = now_tick + 30;
    }
}

static void peer_receive(const uint8_t *s, uint16_t len) {
    uint16_t sport = be16(s), dport = be16(s + 2);
    uint32_t seq = be32(s + 4), ack = be32(s + 8);
    uint8_t flags = s[13];
    uint32_t doff = (uint32_t)(s[12] >> 4) * 4, plen = len - doff;
    if (flags & 0x04) { peer.state = 0; return; }
    if ((flags & 0x02) && !(flags & 0x10)) {               /* SYN: someone connects to the peer */
        if (dport == peer.closed_port) {
            uint8_t r[20] = {0};
            put16(r, dport); put16(r + 2, sport); put32(r + 8, seq + 1);
            r[12] = 5 << 4; r[13] = 0x14;
            put16(r + 16, csum(PEER, ME, 6, r, 20));
            link_put(r, 20, 0);
            return;
        }
        peer.state = 1; peer.port = dport; peer.our_port = sport;
        peer.rcv_nxt = seq + 1; peer.iss = 7000; peer.snd_una = peer.snd_nxt = peer.iss + 1;
        peer.wnd = be16(s + 14);
        peer_send(0x12, peer.iss, 0, 0, dport, sport);
        return;
    }
    if ((flags & 0x12) == 0x12) {                           /* SYN-ACK to the peer's own connect */
        peer.rcv_nxt = seq + 1; peer.snd_una = ack; peer.state = 1; peer.wnd = be16(s + 14);
        peer_send(0x10, peer.snd_nxt, 0, 0, peer.port, peer.our_port);
        peer_output();
        return;
    }
    if (flags & 0x10) {
        if ((int32_t)(ack - peer.snd_una) > 0 && (int32_t)(ack - peer.snd_nxt) <= 0) { peer.snd_una = ack; peer.rto_at = peer.snd_una == peer.snd_nxt ? 0 : now_tick + 30; }
        peer.wnd = be16(s + 14);
    }
    if (plen) {
        if (seq == peer.rcv_nxt) {
            memcpy(peer.in + peer.in_len, s + doff, plen);
            peer.in_len += plen;
            peer.rcv_nxt += plen;
        }
        peer_send(0x10, peer.snd_nxt, 0, 0, peer.port, peer.our_port);
    }
    if ((flags & 0x01) && seq + plen == peer.rcv_nxt) {
        peer.rcv_nxt++;
        peer.fin_rcvd = 1;
        peer_send(0x10, peer.snd_nxt, 0, 0, peer.port, peer.our_port);
        peer.fin_send = 1;
    }
    peer_output();
}

static void peer_tick(void) {
    if (peer.rto_at && now_tick >= peer.rto_at && peer.snd_una != peer.snd_nxt) {
        peer.snd_nxt = peer.snd_una;                       /* go back */
        if (peer.fin_sent && peer.snd_nxt - (peer.iss + 1) <= peer.out_len) peer.fin_sent = 0;
        peer.rto_at = 0;
        peer_output();
    }
}

/* One tick: deliver what is due both ways, run the timers. */
void net_wait(void) {
    now_tick++;
    for (int i = 0; i < nlink;) {
        if (link_q[i].at > now_tick) { i++; continue; }
        pkt_t p = link_q[i];
        link_q[i] = link_q[--nlink];
        if (p.to_peer) peer_receive(p.d, p.len);
        else {
            uint8_t frame[1700];
            memset(frame, 0x22, 14);
            uint8_t *ip = frame + 14;
            memset(ip, 0, 20);
            ip[0] = 0x45; ip[9] = 6;
            memcpy(ip + 12, PEER, 4); memcpy(ip + 16, ME, 4);
            memcpy(ip + 20, p.d, p.len);
            tcp_input(frame, ip, 20, (uint16_t)(20 + p.len));
        }
    }
    peer_tick();
    tcp_tick();
}

static void reset_peer(void) {
    free(peer.out); free(peer.in);
    memset(&peer, 0, sizeof(peer));
    peer.in = malloc(1 << 22);
    peer.closed_port = 81;
}

int main(void) {
    static uint8_t big[300000], got[300000];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 7 + i / 251);

    for (int round = 0; round < 3; round++) {
        drop_pct = round == 0 ? 0 : round == 1 ? 5 : 15;
        jitter = round == 0 ? 0 : 6;
        char what[100];
        reset_peer();
        snprintf(what, sizeof(what), "[%d%% loss, jitter %d] connect", drop_pct, jitter);
        int s = tcp_connect(PEER, 80, 30000);
        CHECK(s >= 0, what);
        if (s < 0) continue;

        /* We send 300 KB: more than the 64 KiB send buffer, so tcp_send waits. */
        int r = tcp_send(s, big, sizeof(big));
        snprintf(what, sizeof(what), "[%d%%] send 300 KB returns all of it", drop_pct);
        CHECK(r == (int)sizeof(big), what);
        for (int i = 0; i < 20000 && peer.in_len < sizeof(big); i++) net_wait();
        snprintf(what, sizeof(what), "[%d%%] peer received every byte in order", drop_pct);
        CHECK(peer.in_len == sizeof(big) && !memcmp(peer.in, big, sizeof(big)), what);

        /* The peer sends 300 KB back, then closes. */
        peer.out = malloc(sizeof(big));
        memcpy(peer.out, big, sizeof(big));
        peer.out_len = sizeof(big);
        peer.fin_send = 1;
        peer_output();
        uint32_t total = 0;
        for (;;) {
            int n = tcp_recv(s, got + total, (uint32_t)(sizeof(got) - total), 60000);
            if (n <= 0) { r = n; break; }
            total += (uint32_t)n;
        }
        snprintf(what, sizeof(what), "[%d%%] received 300 KB intact, then end of data (%u, %d)", drop_pct, total, r);
        CHECK(total == sizeof(big) && !memcmp(got, big, sizeof(big)) && r == 0, what);
        tcp_close(s);
        for (int i = 0; i < 3000; i++) net_wait();
        snprintf(what, sizeof(what), "[%d%%] our FIN reached the peer", drop_pct);
        CHECK(peer.fin_rcvd, what);
    }

    drop_pct = 0; jitter = 0;
    reset_peer();
    int s = tcp_connect(PEER, 81, 5000);
    CHECK(s == TCP_ERR_REFUSED, "closed port: connection refused");

    /* Listening: the peer connects to us. */
    reset_peer();
    int l = tcp_listen(8080);
    CHECK(l >= 0, "listen on 8080");
    CHECK(tcp_listen(8080) == TCP_ERR_INUSE, "second listen on 8080 refused");
    peer.port = 5555; peer.our_port = 8080; peer.iss = 9000; peer.snd_una = peer.snd_nxt = peer.iss;
    peer_send(0x02, peer.iss, 0, 0, 5555, 8080);
    peer.snd_nxt = peer.snd_una = peer.iss + 1;
    int c = tcp_accept(l, 3000);
    CHECK(c >= 0, "accept a connection");
    peer.out = malloc(5);
    memcpy(peer.out, "hello", 5);
    peer.out_len = 5;
    peer_output();
    char hb[16] = {0};
    int n = c >= 0 ? tcp_recv(c, hb, sizeof(hb), 3000) : -1;
    CHECK(n == 5 && !memcmp(hb, "hello", 5), "server side receives \"hello\"");
    CHECK(c >= 0 && tcp_send(c, "world", 5) == 5, "server side answers");
    for (int i = 0; i < 50; i++) net_wait();
    CHECK(peer.in_len == 5 && !memcmp(peer.in, "world", 5), "peer receives \"world\"");
    tcp_close(c);
    tcp_close(l);
    for (int i = 0; i < 1000; i++) net_wait();

    /* Nothing leaks: every socket can be opened again. */
    int opened = 0;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) if (tcp_listen((uint16_t)(9000 + i)) >= 0) opened++;
    CHECK(opened == TCP_MAX_SOCKETS, "all sockets free again after closing");
    if (opened != TCP_MAX_SOCKETS) tcp_print_sockets();

    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures != 0;
}
