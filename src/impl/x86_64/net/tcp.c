// tcp.c - TCP connections (see net/tcp.h)
//
// Each connection is a TCB in a fixed table. The receive path (tcp_input,
// from the network card's poll in the timer interrupt) and the timer
// (tcp_tick) change TCBs; so do the calls below, with interrupts held off
// while they do. Waiting calls sleep in net_wait() between checks.
#include "net/tcp.h"
#include "net/net_internal.h"
#include "drivers/heap.h"
#include "drivers/timer.h"
#include "lib/print.h"
#include "lib/string.h"

enum { S_FREE, S_CLOSED, S_LISTEN, S_SYN_SENT, S_SYN_RCVD, S_ESTABLISHED, S_FIN_WAIT_1, S_FIN_WAIT_2,
       S_CLOSE_WAIT, S_CLOSING, S_LAST_ACK, S_TIME_WAIT };
static const char *const state_names[] = { "free", "closed", "listen", "syn-sent", "syn-rcvd", "established",
                                           "fin-wait-1", "fin-wait-2", "close-wait", "closing", "last-ack",
                                           "time-wait" };

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

#define BUF_SIZE        65536u
#define OUR_MSS         1460
#define RTO_INIT        TIMER_FREQ               /* 1 s */
#define RTO_MAX         (TIMER_FREQ * 16)
#define MAX_RETRIES     8
#define TIME_WAIT_TICKS (TIMER_FREQ * 2)
#define BACKLOG         4

typedef struct {
    int state;
    int app_open;                          /* a caller holds this socket number */
    int owner, parent, accepted, err;
    uint8_t rip[4], rmac[6];
    uint16_t lport, rport, mss;
    uint32_t iss, snd_una, snd_nxt, snd_wnd, rcv_nxt;
    uint8_t *rx;                           /* ring: rx_head - rx_tail bytes waiting */
    uint32_t rx_head, rx_tail;
    uint8_t *tx;                           /* bytes from snd_una on, sent or not */
    uint32_t tx_len;
    int fin_queued, fin_sent, fin_acked, peer_fin;
    uint32_t fin_seq, snd_max;             /* our FIN's sequence number; highest sequence sent */
    uint32_t timer, rto, retries, dupacks, timewait_until;
    uint16_t last_wnd;
} tcb_t;

static tcb_t socks[TCP_MAX_SOCKETS];
static uint16_t next_port = 49152;
static int (*interrupted)(void);

void tcp_set_interrupt_check(int (*check)(void)) { interrupted = check; }

static int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static uint32_t rx_free(const tcb_t *t) { return t->rx ? BUF_SIZE - (t->rx_head - t->rx_tail) : 0; }

/* ---- Sending segments ------------------------------------------------------------- */

static void send_segment(tcb_t *t, uint8_t flags, uint32_t seq, const uint8_t *data, uint32_t len) {
    static uint8_t seg[24 + OUR_MSS];
    int opt = (flags & F_SYN) ? 4 : 0;
    uint32_t wnd = rx_free(t);
    if (wnd > 65535) wnd = 65535;
    memset(seg, 0, 20);
    put16(seg, t->lport);
    put16(seg + 2, t->rport);
    put32(seg + 4, seq);
    put32(seg + 8, (flags & F_ACK) ? t->rcv_nxt : 0);
    seg[12] = (uint8_t)((5 + opt / 4) << 4);
    seg[13] = flags;
    put16(seg + 14, (uint16_t)wnd);
    if (opt) {                                              /* MSS option */
        seg[20] = 2;
        seg[21] = 4;
        put16(seg + 22, OUR_MSS);
    }
    if (len) memcpy(seg + 20 + opt, data, len);
    uint16_t n = (uint16_t)(20 + opt + len);
    put16(seg + 16, net_l4_checksum(net_my_ip(), t->rip, IP_PROTO_TCP, seg, n));
    net_send_ip(t->rmac, t->rip, IP_PROTO_TCP, seg, n);
    t->last_wnd = (uint16_t)wnd;
}

/* A reset for a segment that has no connection. */
static void send_reset(const uint8_t mac[6], const uint8_t ip[4], uint16_t lport, uint16_t rport, uint32_t seq,
                       uint32_t ack, int with_ack) {
    tcb_t t;
    memset(&t, 0, sizeof(t));
    memcpy(t.rmac, mac, 6);
    memcpy(t.rip, ip, 4);
    t.lport = lport;
    t.rport = rport;
    t.rcv_nxt = ack;
    send_segment(&t, (uint8_t)(F_RST | (with_ack ? F_ACK : 0)), seq, 0, 0);
}

static void arm_timer(tcb_t *t) {
    if (t->snd_nxt != t->snd_una || t->snd_max != t->snd_una) {
        if (!t->timer) t->timer = get_tick() + t->rto;
    } else {
        t->timer = 0;
    }
}

static int can_send_data(const tcb_t *t) {
    return t->state == S_ESTABLISHED || t->state == S_CLOSE_WAIT;
}

/* Data may still go out: before our FIN, or resent after it (FIN states). */
static int sending_state(const tcb_t *t) {
    return can_send_data(t) || t->state == S_FIN_WAIT_1 || t->state == S_CLOSING || t->state == S_LAST_ACK;
}

/* Send what the window allows from snd_nxt on, then the FIN when it is due
 * (or again, after going back). */
static void output(tcb_t *t) {
    if (!sending_state(t)) return;
    uint32_t data_end = t->snd_una + t->tx_len;             /* sequence after the last data byte */
    while (seq_lt(t->snd_nxt, data_end) && t->snd_nxt - t->snd_una < t->snd_wnd) {
        uint32_t off = t->snd_nxt - t->snd_una, n = t->tx_len - off;
        uint32_t room = t->snd_wnd - (t->snd_nxt - t->snd_una);
        if (n > t->mss) n = t->mss;
        if (n > room) n = room;
        send_segment(t, F_ACK | F_PSH, t->snd_nxt, t->tx + off, n);
        t->snd_nxt += n;
    }
    if (t->fin_queued && t->snd_nxt == data_end) {
        if (!t->fin_sent) {
            t->fin_sent = 1;
            t->fin_seq = data_end;
            t->state = t->state == S_ESTABLISHED ? S_FIN_WAIT_1 : S_LAST_ACK;
        }
        if (t->snd_nxt == t->fin_seq && !t->fin_acked) {
            send_segment(t, F_FIN | F_ACK, t->fin_seq, 0, 0);
            t->snd_nxt = t->fin_seq + 1;
        }
    }
    if (seq_lt(t->snd_max, t->snd_nxt)) t->snd_max = t->snd_nxt;
    if (!t->timer && t->snd_nxt == t->snd_una && t->tx_len && t->snd_wnd == 0)
        t->timer = get_tick() + t->rto;                     /* zero window: probe later */
    arm_timer(t);
}

/* Resend everything not acknowledged (a peer may have dropped the segments
 * after a lost one). The FIN goes again too, after the data. */
static void go_back(tcb_t *t) {
    t->snd_nxt = t->snd_una;
    uint32_t timer = t->timer;
    t->timer = 0;
    output(t);
    if (t->snd_nxt != t->snd_una && timer) t->timer = timer;   /* keep the backed-off deadline */
}

/* ---- The table ---------------------------------------------------------------------- */

static void release(tcb_t *t) {
    if (t->rx) kfree(t->rx);
    if (t->tx) kfree(t->tx);
    memset(t, 0, sizeof(*t));
}

/* A TCB nobody holds and that has finished goes back to the table. */
static void maybe_release(tcb_t *t) {
    if (t->state == S_FREE) return;
    /* (A connection reset before anyone accepted it is nobody's either.) */
    if ((t->state == S_CLOSED || t->state == S_LISTEN) && !t->app_open) release(t);
}

static tcb_t *alloc_tcb(void) {
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (socks[i].state != S_FREE) continue;
        tcb_t *t = &socks[i];
        memset(t, 0, sizeof(*t));
        t->rx = kmalloc(BUF_SIZE);
        t->tx = kmalloc(BUF_SIZE);
        if (!t->rx || !t->tx) {
            release(t);
            return 0;
        }
        t->state = S_CLOSED;
        t->parent = -1;
        t->mss = 536;
        t->rto = RTO_INIT;
        t->iss = (uint32_t)get_tick() * 2654435761u ^ (uint32_t)(i * 7919);
        return t;
    }
    return 0;
}

static void set_closed(tcb_t *t, int err) {
    t->state = S_CLOSED;
    t->timer = 0;
    if (err && !t->err) t->err = err;
    maybe_release(t);
}

static tcb_t *get(int s) {
    if (s < 0 || s >= TCP_MAX_SOCKETS) return 0;
    tcb_t *t = &socks[s];
    return t->state != S_FREE && t->app_open ? t : 0;
}

/* ---- Receiving ------------------------------------------------------------------------ */

static uint16_t mss_option(const uint8_t *opt, int len) {
    for (int i = 0; i < len;) {
        if (opt[i] == 0) break;
        if (opt[i] == 1) { i++; continue; }
        if (i + 1 >= len || opt[i + 1] < 2) break;
        if (opt[i] == 2 && opt[i + 1] == 4 && i + 3 < len) return be16(opt + i + 2);
        i += opt[i + 1];
    }
    return 536;
}

void tcp_input(const uint8_t *frame, const uint8_t *ip, uint16_t ihl, uint16_t total) {
    const uint8_t *seg = ip + ihl, *src = ip + 12;
    uint16_t len = (uint16_t)(total - ihl);
    if (len < 20 || net_l4_checksum(src, ip + 16, IP_PROTO_TCP, seg, len) != 0) return;
    uint16_t sport = be16(seg), dport = be16(seg + 2);
    uint32_t seq = be32(seg + 4), ack = be32(seg + 8);
    uint32_t doff = (uint32_t)(seg[12] >> 4) * 4;
    uint8_t flags = seg[13];
    uint32_t wnd = be16(seg + 14);
    if (doff < 20 || doff > len) return;
    const uint8_t *data = seg + doff;
    uint32_t plen = len - doff;
    const uint8_t *src_mac = frame + 6;

    tcb_t *t = 0, *listener = 0;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        tcb_t *c = &socks[i];
        if (c->state == S_FREE || c->lport != dport) continue;
        if (c->state == S_LISTEN) listener = c;
        else if (c->rport == sport && !memcmp(c->rip, src, 4) && c->state != S_CLOSED) t = c;
    }
    if (!t && listener && (flags & (F_SYN | F_ACK | F_RST)) == F_SYN) {
        int pending = 0, lidx = (int)(listener - socks);
        for (int i = 0; i < TCP_MAX_SOCKETS; i++)
            if (socks[i].state != S_FREE && socks[i].parent == lidx && !socks[i].accepted) pending++;
        if (pending >= BACKLOG || !(t = alloc_tcb())) return;          /* the peer will retry */
        memcpy(t->rip, src, 4);
        memcpy(t->rmac, src_mac, 6);
        t->lport = dport;
        t->rport = sport;
        t->parent = lidx;
        t->owner = listener->owner;
        t->rcv_nxt = seq + 1;
        t->snd_una = t->iss;
        t->snd_nxt = t->snd_max = t->iss + 1;
        t->snd_wnd = wnd;
        t->mss = mss_option(seg + 20, (int)doff - 20);
        if (t->mss > OUR_MSS) t->mss = OUR_MSS;
        t->state = S_SYN_RCVD;
        send_segment(t, F_SYN | F_ACK, t->iss, 0, 0);
        arm_timer(t);
        return;
    }
    if (!t) {                                               /* nobody here: reset */
        if (!(flags & F_RST))
            send_reset(src_mac, src, dport, sport, (flags & F_ACK) ? ack : 0,
                       seq + plen + ((flags & F_SYN) ? 1 : 0) + ((flags & F_FIN) ? 1 : 0), !(flags & F_ACK));
        return;
    }

    if (flags & F_RST) {
        if (t->state == S_SYN_SENT) {
            if ((flags & F_ACK) && ack == t->snd_nxt) set_closed(t, TCP_ERR_REFUSED);
        } else if (seq_le(t->rcv_nxt, seq) && seq_lt(seq, t->rcv_nxt + 65536)) {
            set_closed(t, TCP_ERR_RESET);
        }
        return;
    }

    if (t->state == S_SYN_SENT) {
        if ((flags & F_ACK) && ack != t->snd_nxt) {
            send_reset(t->rmac, t->rip, t->lport, t->rport, ack, 0, 0);
            return;
        }
        if (!(flags & F_SYN) || !(flags & F_ACK)) return;
        t->rcv_nxt = seq + 1;
        t->snd_una = ack;
        t->snd_wnd = wnd;
        t->mss = mss_option(seg + 20, (int)doff - 20);
        if (t->mss > OUR_MSS) t->mss = OUR_MSS;
        t->state = S_ESTABLISHED;
        t->timer = 0;
        t->retries = 0;
        send_segment(t, F_ACK, t->snd_nxt, 0, 0);
        output(t);
        return;
    }

    /* Synchronized states. Data must start at rcv_nxt (trim what we have). */
    if (flags & F_SYN) {                                    /* a resent SYN(-ACK): our ACK was lost */
        send_segment(t, F_ACK, t->snd_nxt, 0, 0);
        return;
    }
    if ((plen || (flags & F_FIN)) && seq != t->rcv_nxt) {
        uint32_t skip = t->rcv_nxt - seq;
        if (seq_lt(seq, t->rcv_nxt) && skip < plen) {       /* partly new: keep the new part */
            data += skip;
            plen -= skip;
            seq = t->rcv_nxt;
        } else {
            send_segment(t, F_ACK, t->snd_nxt, 0, 0);       /* old or out of order: say what we want */
            return;
        }
    }
    if (!(flags & F_ACK)) return;

    if (t->state == S_SYN_RCVD) {
        if (ack != t->snd_nxt) {
            send_reset(t->rmac, t->rip, t->lport, t->rport, ack, 0, 0);
            return;
        }
        t->snd_una = ack;
        t->state = S_ESTABLISHED;
        t->timer = 0;
        t->retries = 0;
    }

    /* Acknowledgement of what we sent. */
    if (seq_lt(t->snd_una, ack) && seq_le(ack, t->snd_max)) {
        uint32_t acked = ack - t->snd_una;
        uint32_t data_acked = acked > t->tx_len ? t->tx_len : acked;
        if (data_acked) {
            memmove(t->tx, t->tx + data_acked, t->tx_len - data_acked);
            t->tx_len -= data_acked;
        }
        if (t->fin_sent && ack == t->fin_seq + 1) t->fin_acked = 1;
        t->snd_una = ack;
        if (seq_lt(t->snd_nxt, ack)) t->snd_nxt = ack;
        t->snd_wnd = wnd;
        t->retries = 0;
        t->dupacks = 0;
        t->rto = RTO_INIT;
        t->timer = 0;
    } else if (ack == t->snd_una) {
        if (!plen && wnd == t->snd_wnd && t->snd_max != t->snd_una && ++t->dupacks == 3) {
            go_back(t);                                    /* fast retransmit */
        }
        t->snd_wnd = wnd;
    }
    if (t->fin_acked) {
        if (t->state == S_FIN_WAIT_1) t->state = S_FIN_WAIT_2;
        else if (t->state == S_CLOSING) { t->state = S_TIME_WAIT; t->timewait_until = get_tick() + TIME_WAIT_TICKS; }
        else if (t->state == S_LAST_ACK) { set_closed(t, 0); return; }
    }

    /* Data. */
    int need_ack = 0;
    if (plen && (t->state == S_ESTABLISHED || t->state == S_FIN_WAIT_1 || t->state == S_FIN_WAIT_2)) {
        uint32_t room = rx_free(t), n = plen < room ? plen : room;
        for (uint32_t i = 0; i < n; i++) t->rx[(t->rx_head + i) % BUF_SIZE] = data[i];
        t->rx_head += n;
        t->rcv_nxt += n;
        if (n < plen) flags &= (uint8_t)~F_FIN;             /* the rest (and FIN) comes again */
        need_ack = 1;
    }
    if ((flags & F_FIN) && seq + plen == t->rcv_nxt && !t->peer_fin) {
        t->rcv_nxt++;
        t->peer_fin = 1;
        need_ack = 1;
        if (t->state == S_ESTABLISHED || t->state == S_SYN_RCVD) t->state = S_CLOSE_WAIT;
        else if (t->state == S_FIN_WAIT_1) {
            t->state = t->fin_acked ? S_TIME_WAIT : S_CLOSING;
            if (t->fin_acked) t->timewait_until = get_tick() + TIME_WAIT_TICKS;
        } else if (t->state == S_FIN_WAIT_2) {
            t->state = S_TIME_WAIT;
            t->timewait_until = get_tick() + TIME_WAIT_TICKS;
        }
    }
    if (need_ack) send_segment(t, F_ACK, t->snd_nxt, 0, 0);
    output(t);
}

/* ---- Timers ---------------------------------------------------------------------------- */

void tcp_tick(void) {
    uint64_t fl = net_irq_save();
    uint32_t now = get_tick();
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        tcb_t *t = &socks[i];
        if (t->state == S_FREE) continue;
        if (t->state == S_TIME_WAIT && (int32_t)(now - t->timewait_until) >= 0) {
            set_closed(t, 0);
            continue;
        }
        if (!t->timer || (int32_t)(now - t->timer) < 0) continue;
        if (++t->retries > MAX_RETRIES) {
            if (t->state != S_SYN_SENT) send_segment(t, F_RST | F_ACK, t->snd_nxt, 0, 0);
            set_closed(t, TCP_ERR_TIMEOUT);
            continue;
        }
        t->rto = t->rto * 2 > RTO_MAX ? RTO_MAX : t->rto * 2;
        t->timer = now + t->rto;
        if (t->state == S_SYN_SENT) {
            send_segment(t, F_SYN, t->iss, 0, 0);
        } else if (t->state == S_SYN_RCVD) {
            send_segment(t, F_SYN | F_ACK, t->iss, 0, 0);
        } else if (t->snd_max != t->snd_una) {
            /* Go back: everything not acknowledged is sent again (a peer that
             * dropped the segments after a lost one needs all of them). */
            go_back(t);
        } else if (t->tx_len && t->snd_wnd == 0) {           /* zero-window probe: one byte */
            send_segment(t, F_ACK, t->snd_una, t->tx, 1);
        } else {
            t->timer = 0;
        }
    }
    net_irq_restore(fl);
}

/* ---- The calls ---------------------------------------------------------------------------- */

static int stop_waiting(void) { return interrupted && interrupted(); }

int tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms) {
    uint8_t mac[6];
    if (net_next_hop_mac(ip, mac) != 0) return TCP_ERR_NOROUTE;
    uint64_t fl = net_irq_save();
    tcb_t *t = alloc_tcb();
    if (!t) {
        net_irq_restore(fl);
        return TCP_ERR_NOSOCK;
    }
    memcpy(t->rip, ip, 4);
    memcpy(t->rmac, mac, 6);
    t->rport = port;
    t->lport = next_port;
    next_port = next_port >= 65000 ? 49152 : (uint16_t)(next_port + 1);
    t->app_open = 1;
    t->snd_una = t->iss;
    t->snd_nxt = t->snd_max = t->iss + 1;
    t->state = S_SYN_SENT;
    send_segment(t, F_SYN, t->iss, 0, 0);
    arm_timer(t);
    int s = (int)(t - socks);
    net_irq_restore(fl);

    uint32_t start = get_tick(), ticks = timeout_ms * TIMER_FREQ / 1000 + 1;
    while (t->state == S_SYN_SENT && (uint32_t)(get_tick() - start) < ticks && !stop_waiting()) {
        tcp_tick();
        net_wait();
    }
    if (t->state == S_ESTABLISHED || t->state == S_CLOSE_WAIT) return s;
    int err = t->err ? t->err : stop_waiting() ? TCP_ERR_INTR : TCP_ERR_TIMEOUT;
    tcp_close(s);
    return err;
}

int tcp_listen(uint16_t port) {
    uint64_t fl = net_irq_save();
    for (int i = 0; i < TCP_MAX_SOCKETS; i++)
        if (socks[i].state == S_LISTEN && socks[i].lport == port) {
            net_irq_restore(fl);
            return TCP_ERR_INUSE;
        }
    tcb_t *t = alloc_tcb();
    if (!t) {
        net_irq_restore(fl);
        return TCP_ERR_NOSOCK;
    }
    kfree(t->rx);                                           /* a listener holds no data */
    kfree(t->tx);
    t->rx = t->tx = 0;
    t->lport = port;
    t->state = S_LISTEN;
    t->app_open = 1;
    net_irq_restore(fl);
    return (int)(t - socks);
}

int tcp_accept(int listener, uint32_t timeout_ms) {
    tcb_t *l = get(listener);
    if (!l || l->state != S_LISTEN) return TCP_ERR_NOSOCK;
    uint32_t start = get_tick(), ticks = timeout_ms * TIMER_FREQ / 1000;
    for (;;) {
        uint64_t fl = net_irq_save();
        for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
            tcb_t *c = &socks[i];
            if (c->state == S_FREE || c->parent != listener || c->accepted) continue;
            if (c->state == S_ESTABLISHED || c->state == S_CLOSE_WAIT) {
                c->accepted = 1;
                c->app_open = 1;
                net_irq_restore(fl);
                return i;
            }
        }
        net_irq_restore(fl);
        if ((uint32_t)(get_tick() - start) >= ticks) return TCP_ERR_TIMEOUT;
        if (stop_waiting()) return TCP_ERR_INTR;
        tcp_tick();
        net_wait();
    }
}

int tcp_send(int s, const void *buf, uint32_t len) {
    tcb_t *t = get(s);
    if (!t) return TCP_ERR_NOSOCK;
    const uint8_t *p = buf;
    uint32_t done = 0, idle = get_tick();
    while (done < len) {
        uint64_t fl = net_irq_save();
        if (!can_send_data(t) || t->fin_queued) {
            int err = t->err ? t->err : TCP_ERR_CLOSED;
            net_irq_restore(fl);
            return done ? (int)done : err;
        }
        uint32_t room = BUF_SIZE - t->tx_len, n = len - done < room ? len - done : room;
        if (n) {
            memcpy(t->tx + t->tx_len, p + done, n);
            t->tx_len += n;
            done += n;
            output(t);
            idle = get_tick();
        }
        net_irq_restore(fl);
        if (done < len) {
            if (stop_waiting()) return done ? (int)done : TCP_ERR_INTR;
            if ((uint32_t)(get_tick() - idle) > 60 * TIMER_FREQ) return done ? (int)done : TCP_ERR_TIMEOUT;
            tcp_tick();
            net_wait();
        }
    }
    return (int)done;
}

uint32_t tcp_available(int s) {
    tcb_t *t = get(s);
    return t && t->rx ? t->rx_head - t->rx_tail : 0;
}

int tcp_recv(int s, void *buf, uint32_t len, uint32_t timeout_ms) {
    tcb_t *t = get(s);
    if (!t || !t->rx) return TCP_ERR_NOSOCK;
    uint32_t start = get_tick(), ticks = timeout_ms * TIMER_FREQ / 1000;
    for (;;) {
        uint64_t fl = net_irq_save();
        uint32_t avail = t->rx_head - t->rx_tail;
        if (avail) {
            uint32_t n = avail < len ? avail : len;
            uint8_t *out = buf;
            for (uint32_t i = 0; i < n; i++) out[i] = t->rx[(t->rx_tail + i) % BUF_SIZE];
            t->rx_tail += n;
            /* The window had (nearly) closed and is open again: tell the peer. */
            if (t->last_wnd < 2 * OUR_MSS && rx_free(t) >= 8 * OUR_MSS && can_send_data(t))
                send_segment(t, F_ACK, t->snd_nxt, 0, 0);
            net_irq_restore(fl);
            return (int)n;
        }
        int ended = t->peer_fin || t->state == S_CLOSED;
        int err = t->err;
        net_irq_restore(fl);
        if (ended) return err && err != TCP_ERR_TIMEOUT ? err : 0;
        if ((uint32_t)(get_tick() - start) >= ticks) return TCP_ERR_TIMEOUT;
        if (stop_waiting()) return TCP_ERR_INTR;
        tcp_tick();
        net_wait();
    }
}

int tcp_close(int s) {
    tcb_t *t = get(s);
    if (!t) return TCP_ERR_NOSOCK;
    uint64_t fl = net_irq_save();
    t->app_open = 0;
    if (t->state == S_LISTEN) {
        t->state = S_CLOSED;
        for (int i = 0; i < TCP_MAX_SOCKETS; i++)          /* connections not accepted yet: reset */
            if (socks[i].state != S_FREE && socks[i].parent == s && !socks[i].accepted) {
                if (socks[i].state != S_CLOSED) send_segment(&socks[i], F_RST | F_ACK, socks[i].snd_nxt, 0, 0);
                socks[i].accepted = 1;
                set_closed(&socks[i], 0);
            }
    } else if (t->state == S_SYN_SENT || t->state == S_SYN_RCVD) {
        set_closed(t, 0);
    } else if (can_send_data(t)) {
        t->fin_queued = 1;
        output(t);
    }
    maybe_release(t);
    net_irq_restore(fl);
    return 0;
}

void tcp_set_owner(int s, int pid) {
    if (s >= 0 && s < TCP_MAX_SOCKETS) socks[s].owner = pid;
}

int tcp_owner(int s) {
    tcb_t *t = get(s);
    return t ? t->owner : -1;
}

void tcp_close_owned_by(int pid) {
    for (int i = 0; i < TCP_MAX_SOCKETS; i++)
        if (socks[i].state != S_FREE && socks[i].app_open && socks[i].owner == pid) tcp_close(i);
}

void tcp_print_sockets(void) {
    int any = 0;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        tcb_t *t = &socks[i];
        if (t->state == S_FREE) continue;
        if (!any) kprintf("  #   local   remote                 state        queued in/out\n");
        any = 1;
        if (t->state == S_LISTEN) {
            kprintf("  %-2d  %-5u   *                      listen\n", i, t->lport);
            continue;
        }
        char remote[24];
        k_snprintf(remote, sizeof(remote), "%u.%u.%u.%u:%u", t->rip[0], t->rip[1], t->rip[2], t->rip[3], t->rport);
        kprintf("  %-2d  %-5u   %-22s %-12s %u/%u\n", i, t->lport, remote, state_names[t->state],
                t->rx_head - t->rx_tail, t->tx_len);
    }
    if (!any) kprintf("  no TCP connections\n");
}
