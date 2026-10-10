// hda.c - Intel High Definition Audio (spec revision 1.0a)
//
//   1. PCI: find the controller (class 04/03, or Intel's 04/01 "smart sound"
//      DSP, whose BAR 0 is the same HDA register block), memory + bus master.
//   2. Reset the controller; the codecs announce themselves in STATESTS.
//   3. Command rings: CORB (verbs out), RIRB (responses in), polled.
//   4. Each codec's audio function group: read every widget (type,
//      capabilities, connections, pin configuration). Pick output pins
//      (internal speaker first, then headphones and line out) and find a path
//      from each back to a DAC.
//   5. Power up, unmute and select along those paths, enable the pins (and
//      their external amplifiers, EAPD), and point the DACs at stream 1.
//   6. Playback: one output stream descriptor, a buffer descriptor list over
//      a ring of 4 x 16 KiB, refilled while the DMA position (LPIB) moves on.
#include "drivers/hda.h"
#include "drivers/pci.h"
#include "drivers/paging.h"
#include "drivers/heap.h"
#include "drivers/timer.h"
#include "drivers/keyboard.h"
#include "lib/print.h"
#include "lib/string.h"
#include <stdint.h>

/* Controller registers */
#define GCAP       0x00
#define GCTL       0x08
#define STATESTS   0x0E
#define INTCTL     0x20
#define CORBLBASE  0x40
#define CORBUBASE  0x44
#define CORBWP     0x48
#define CORBRP     0x4A
#define CORBCTL    0x4C
#define CORBSIZE   0x4E
#define RIRBLBASE  0x50
#define RIRBUBASE  0x54
#define RIRBWP     0x58
#define RINTCNT    0x5A
#define RIRBCTL    0x5C
#define RIRBSTS    0x5D
#define RIRBSIZE   0x5E
#define ICOI       0x60
#define ICII       0x64
#define ICIS       0x68

/* Stream descriptor registers (offset from the descriptor) */
#define SD_CTL     0x00
#define SD_STS     0x03
#define SD_LPIB    0x04
#define SD_CBL     0x08
#define SD_LVI     0x0C
#define SD_FMT     0x12
#define SD_BDPL    0x18
#define SD_BDPU    0x1C

/* Verbs */
#define V_GET_PARAM     0xF00
#define V_GET_CONN      0xF02
#define V_SET_SELECT    0x701
#define V_SET_POWER     0x705
#define V_SET_STREAM    0x706
#define V_SET_PINCTL    0x707
#define V_SET_EAPD      0x70C
#define V_GET_CONFIG    0xF1C
#define V_SET_FORMAT    0x2          /* 4-bit verbs, 16-bit payload */
#define V_SET_AMP       0x3

#define P_VENDOR        0x00
#define P_NODES         0x04
#define P_FG_TYPE       0x05
#define P_WIDGET_CAPS   0x09
#define P_PIN_CAPS      0x0C
#define P_IN_AMP        0x0D
#define P_CONN_LEN      0x0E
#define P_OUT_AMP       0x12

#define W_OUTPUT  0
#define W_INPUT   1
#define W_MIXER   2
#define W_SELECT  3
#define W_PIN     4

#define RATE          48000
#define FORMAT_48K16  0x0011     /* 48 kHz, 16 bits, 2 channels */
#define STREAM_TAG    1
#define RING_BYTES    (64 * 1024)
#define BDL_ENTRIES   4

typedef struct {
    uint8_t type, nconn, used;
    uint32_t caps, pincaps, config, inamp, outamp;
    uint8_t conn[16];
} widget_t;

#define MAX_NODES 128
#define MAX_PATH  6
#define MAX_OUTS  3

typedef struct {
    uint8_t pin, dac, len;
    uint8_t nodes[MAX_PATH];     /* pin ... dac */
    uint8_t sel[MAX_PATH];       /* connection index taken at nodes[i] towards nodes[i+1] */
} path_t;

static volatile uint8_t *regs;
static int ready, use_immediate;
static uint8_t codec_addr;
static uint32_t codec_vendor, codec_mask;
static uint8_t afg;
static widget_t w[MAX_NODES];
static uint8_t first_nid, num_nids;
static uint32_t afg_inamp, afg_outamp;
static path_t outs[MAX_OUTS];
static int n_outs;
static uint32_t *corb;
static uint32_t *rirb;
static int corb_entries, rirb_rp;
static uint32_t sd;              /* our output stream descriptor's offset */
static uint8_t *ring;
static uint32_t *bdl;
static int volume = 80;
static char ctrl_desc[96];
static uint16_t gcap;

static uint8_t rd8(uint32_t o) { return *(volatile uint8_t *)(regs + o); }
static uint16_t rd16(uint32_t o) { return *(volatile uint16_t *)(regs + o); }
static uint32_t rd32(uint32_t o) { return *(volatile uint32_t *)(regs + o); }
static void wr8(uint32_t o, uint8_t v) { *(volatile uint8_t *)(regs + o) = v; }
static void wr16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(regs + o) = v; }
static void wr32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(regs + o) = v; }

static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void udelay(uint32_t us) {
    static uint64_t per_us;
    if (!per_us) {
        uint32_t t = get_tick();
        while (get_tick() == t) __asm__ volatile("pause");
        uint64_t a = rdtsc();
        t = get_tick();
        while ((uint32_t)(get_tick() - t) < 5) __asm__ volatile("pause");
        per_us = (rdtsc() - a) / (5 * 1000000 / TIMER_FREQ);
        if (!per_us) per_us = 1;
    }
    uint64_t end = rdtsc() + per_us * us;
    while (rdtsc() < end) __asm__ volatile("pause");
}

static void *dma_alloc(uint64_t size, uint64_t align) {
    uint8_t *raw = kmalloc(size + align);
    if (!raw) return 0;
    uint8_t *p = (uint8_t *)(((uint64_t)raw + align - 1) & ~(align - 1));
    memset(p, 0, size);
    return p;
}

/* ---- Commands ----------------------------------------------------------------- */

static int corb_send(uint32_t verb, uint32_t *resp) {
    wr8(RIRBSTS, 0x05);                                     /* acknowledge: some controllers wait for it */
    int wp = (rd16(CORBWP) + 1) % corb_entries;
    corb[wp] = verb;
    __asm__ volatile("mfence" ::: "memory");
    wr16(CORBWP, (uint16_t)wp);
    for (int t = 0; t < 2000; t++) {                        /* up to 20 ms */
        int hw = rd16(RIRBWP) & 0xFF;
        while (rirb_rp != hw) {
            rirb_rp = (rirb_rp + 1) % corb_entries;
            uint32_t r = rirb[rirb_rp * 2], ex = rirb[rirb_rp * 2 + 1];
            if (ex & 0x10) continue;                        /* unsolicited (a jack event) */
            *resp = r;
            wr8(RIRBSTS, 0x05);
            return 0;
        }
        udelay(10);
    }
    return -1;
}

static int immediate_send(uint32_t verb, uint32_t *resp) {
    for (int t = 0; t < 1000 && (rd16(ICIS) & 1); t++) udelay(10);
    wr16(ICIS, 2);                                          /* clear "result valid" */
    wr32(ICOI, verb);
    wr16(ICIS, 1);
    for (int t = 0; t < 2000; t++) {
        if ((rd16(ICIS) & 3) == 2) {
            *resp = rd32(ICII);
            return 0;
        }
        udelay(10);
    }
    return -1;
}

static uint32_t cmd(uint8_t nid, uint32_t verb, uint32_t payload) {
    uint32_t v = (uint32_t)codec_addr << 28 | (uint32_t)nid << 20;
    if (verb < 0x10) v |= verb << 16 | (payload & 0xFFFF);  /* 4-bit verb */
    else v |= verb << 8 | (payload & 0xFF);
    uint32_t r = 0;
    if ((use_immediate ? immediate_send(v, &r) : corb_send(v, &r)) != 0) return 0xFFFFFFFFu;
    return r;
}

static uint32_t param(uint8_t nid, uint32_t p) { return cmd(nid, V_GET_PARAM, p); }

static int setup_rings(void) {
    wr8(CORBCTL, 0);
    wr8(RIRBCTL, 0);
    for (int t = 0; t < 100 && ((rd8(CORBCTL) | rd8(RIRBCTL)) & 2); t++) udelay(10);
    if (!corb) {
        corb = dma_alloc(1024, 128);
        rirb = dma_alloc(2048, 128);
        if (!corb || !rirb) return -1;
    }
    uint8_t cs = rd8(CORBSIZE), rs = rd8(RIRBSIZE);
    int sz = (cs & 0x40) && (rs & 0x40) ? 2 : (cs & 0x20) && (rs & 0x20) ? 1 : 0;
    corb_entries = sz == 2 ? 256 : sz == 1 ? 16 : 2;
    wr8(CORBSIZE, (uint8_t)((cs & ~3) | sz));
    wr8(RIRBSIZE, (uint8_t)((rs & ~3) | sz));
    wr32(CORBLBASE, (uint32_t)(uint64_t)corb);
    wr32(CORBUBASE, (uint32_t)((uint64_t)corb >> 32));
    wr32(RIRBLBASE, (uint32_t)(uint64_t)rirb);
    wr32(RIRBUBASE, (uint32_t)((uint64_t)rirb >> 32));
    wr16(CORBRP, 0x8000);                                   /* reset the read pointer */
    for (int t = 0; t < 100 && !(rd16(CORBRP) & 0x8000); t++) udelay(10);
    wr16(CORBRP, 0);
    for (int t = 0; t < 100 && (rd16(CORBRP) & 0x8000); t++) udelay(10);
    wr16(CORBWP, 0);
    wr16(RIRBWP, 0x8000);
    rirb_rp = 0;
    wr16(RINTCNT, 1);
    wr8(CORBCTL, 2);                                        /* DMA run */
    wr8(RIRBCTL, 3);                                        /* DMA run + response flag (no IRQ: GIE is off) */
    udelay(100);
    return 0;
}

/* ---- Codec graph -------------------------------------------------------------- */

static void read_connections(uint8_t nid) {
    widget_t *x = &w[nid - first_nid];
    uint32_t len = param(nid, P_CONN_LEN);
    int lng = (len >> 7) & 1, n = (int)(len & 0x7F);
    int per = lng ? 2 : 4, bits = lng ? 16 : 8;
    x->nconn = 0;
    int prev = -1;
    for (int i = 0; i < n && x->nconn < 16; i += per) {
        uint32_t r = cmd(nid, V_GET_CONN, (uint32_t)i);
        for (int k = 0; k < per && i + k < n && x->nconn < 16; k++) {
            uint32_t e = (r >> (k * bits)) & (lng ? 0xFFFF : 0xFF);
            uint32_t range = e & (lng ? 0x8000 : 0x80), id = e & (lng ? 0x7FFF : 0x7F);
            if (range && prev >= 0) {                       /* "prev..id" */
                for (uint32_t m = (uint32_t)prev + 1; m <= id && x->nconn < 16; m++) x->conn[x->nconn++] = (uint8_t)m;
            } else {
                x->conn[x->nconn++] = (uint8_t)id;
            }
            prev = (int)id;
        }
    }
}

static widget_t *node(uint8_t nid) {
    return nid >= first_nid && nid < first_nid + num_nids ? &w[nid - first_nid] : 0;
}

/* Depth-first from `nid` to a DAC through mixers and selectors. */
static int find_dac(uint8_t nid, path_t *p, int depth) {
    widget_t *x = node(nid);
    if (!x || depth >= MAX_PATH) return 0;
    p->nodes[depth] = nid;
    p->len = (uint8_t)(depth + 1);
    if (x->type == W_OUTPUT) return !(x->caps & (1u << 9));   /* not a digital converter */
    if (depth > 0 && x->type != W_MIXER && x->type != W_SELECT) return 0;
    for (int i = 0; i < x->nconn; i++) {
        widget_t *y = node(x->conn[i]);
        if (!y || (y->type != W_OUTPUT && y->type != W_MIXER && y->type != W_SELECT)) continue;
        p->sel[depth] = (uint8_t)i;
        if (find_dac(x->conn[i], p, depth + 1)) return 1;
    }
    return 0;
}

static const char *device_name(uint32_t config) {
    static const char *names[16] = { "line out", "speaker", "headphones", "CD", "S/PDIF out", "digital out",
                                     "modem line", "modem handset", "line in", "aux", "microphone", "telephony",
                                     "S/PDIF in", "digital in", "reserved", "other" };
    return names[(config >> 20) & 0xF];
}

/* Output pins worth driving, best first: internal speaker, headphones, line out. */
static int pin_rank(widget_t *x) {
    uint32_t conn = x->config >> 30, dev = (x->config >> 20) & 0xF;
    if (conn == 1 || !(x->pincaps & (1u << 4))) return 0;  /* not connected, cannot output */
    if (x->pincaps & ((1u << 7) | (1u << 24))) return 0;   /* HDMI / DisplayPort */
    if (dev == 1) return conn == 2 ? 4 : 3;
    if (dev == 2) return 2;
    if (dev == 0) return 1;
    return 0;
}

static int scan_codec(uint8_t addr) {
    codec_addr = addr;
    codec_vendor = param(0, P_VENDOR);
    if (codec_vendor == 0xFFFFFFFFu || !codec_vendor) return -1;
    uint32_t sub = param(0, P_NODES);
    afg = 0;
    for (uint32_t n = (sub >> 16) & 0xFF, e = n + (sub & 0xFF); n < e; n++)
        if ((param((uint8_t)n, P_FG_TYPE) & 0xFF) == 1) { afg = (uint8_t)n; break; }
    if (!afg) return -1;
    cmd(afg, V_SET_POWER, 0);                               /* D0 */
    udelay(1000);
    afg_inamp = param(afg, P_IN_AMP);
    afg_outamp = param(afg, P_OUT_AMP);
    uint32_t nodes = param(afg, P_NODES);
    first_nid = (uint8_t)((nodes >> 16) & 0xFF);
    num_nids = (uint8_t)(nodes & 0xFF);
    if (num_nids > MAX_NODES) num_nids = MAX_NODES;
    memset(w, 0, sizeof(w));
    for (int i = 0; i < num_nids; i++) {
        uint8_t nid = (uint8_t)(first_nid + i);
        widget_t *x = &w[i];
        x->caps = param(nid, P_WIDGET_CAPS);
        x->type = (uint8_t)((x->caps >> 20) & 0xF);
        x->inamp = (x->caps & (1u << 3)) ? param(nid, P_IN_AMP) : afg_inamp;
        x->outamp = (x->caps & (1u << 3)) ? param(nid, P_OUT_AMP) : afg_outamp;
        if (x->caps & (1u << 8)) read_connections(nid);
        if (x->type == W_PIN) {
            x->pincaps = param(nid, P_PIN_CAPS);
            x->config = cmd(nid, V_GET_CONFIG, 0);
        }
    }
    /* Paths from the best pins (one per pin; DACs may be shared). */
    n_outs = 0;
    for (int want = 4; want >= 1 && n_outs < MAX_OUTS; want--)
        for (int i = 0; i < num_nids && n_outs < MAX_OUTS; i++) {
            widget_t *x = &w[i];
            if (x->type != W_PIN || pin_rank(x) != want) continue;
            path_t p;
            memset(&p, 0, sizeof(p));
            if (!find_dac((uint8_t)(first_nid + i), &p, 0)) continue;
            p.pin = p.nodes[0];
            p.dac = p.nodes[p.len - 1];
            outs[n_outs++] = p;
        }
    return n_outs ? 0 : -1;
}

/* 0 dB on an amplifier: its offset field. */
static uint32_t zero_db(uint32_t ampcaps) { return ampcaps & 0x7F; }

static void enable_path(const path_t *p) {
    for (int i = 0; i < p->len; i++) {
        uint8_t nid = p->nodes[i];
        widget_t *x = node(nid);
        if (x->caps & (1u << 10)) cmd(nid, V_SET_POWER, 0);  /* power control: D0 */
        if (x->caps & (1u << 2))                            /* output amp: unmute, 0 dB, both sides */
            cmd(nid, V_SET_AMP, 0xB000 | zero_db(x->outamp));
        if (i + 1 < p->len) {
            if (x->type != W_MIXER && x->nconn > 1) cmd(nid, V_SET_SELECT, p->sel[i]);
            if (x->caps & (1u << 1))                        /* input amp on the used input */
                cmd(nid, V_SET_AMP, 0x7000 | (uint32_t)p->sel[i] << 8 | zero_db(x->inamp));
        }
        x->used = 1;
    }
    widget_t *pin = node(p->pin);
    uint32_t ctl = 0x40;                                    /* output enable */
    if (pin->pincaps & (1u << 3)) ctl |= 0x80;              /* headphone amp */
    cmd(p->pin, V_SET_PINCTL, ctl);
    if (pin->pincaps & (1u << 16)) cmd(p->pin, V_SET_EAPD, 0x02);   /* external amplifier on */
    cmd(p->dac, V_SET_FORMAT, FORMAT_48K16);
    cmd(p->dac, V_SET_STREAM, STREAM_TAG << 4);
}

/* ---- Controller --------------------------------------------------------------- */

static int find_controller(pci_device_t *out) {
    static pci_device_t devs[64];
    int n = pci_scan(devs, 64);
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (devs[i].class_code != 0x04) continue;
        if (devs[i].subclass == 0x03) { best = i; break; }  /* HD Audio */
        if (devs[i].subclass == 0x01 && devs[i].vendor == 0x8086 && best < 0) best = i;   /* Intel SST */
    }
    if (best < 0) return -1;
    *out = devs[best];
    return 0;
}

int sound_init(char *how, int size) {
    pci_device_t d;
    if (find_controller(&d) != 0) {
        k_snprintf(how, (size_t)size, "no HD Audio controller");
        return -1;
    }
    uint32_t after;
    pci_power_on(d.bus, d.slot, d.func, &after);
    uint32_t bar0 = pci_config_read_dword(d.bus, d.slot, d.func, 0x10);
    uint64_t base = bar0 & ~0xFull;
    if (((bar0 >> 1) & 3) == 2) base |= (uint64_t)pci_config_read_dword(d.bus, d.slot, d.func, 0x14) << 32;
    if (!base) base = pci_assign_bar0(d.bus, d.slot, d.func);
    if (!base) {
        k_snprintf(how, (size_t)size, "the controller has no address (%s)", pci_assign_note());
        return -1;
    }
    uint32_t c = pci_config_read_dword(d.bus, d.slot, d.func, 0x04);
    pci_config_write_dword(d.bus, d.slot, d.func, 0x04, (c & 0xFFFF) | 0x2 | 0x4 | 0x400);
    if (d.vendor == 0x8086) {                               /* traffic class 0 (Linux: TCSEL) */
        uint32_t t = pci_config_read_dword(d.bus, d.slot, d.func, 0x44);
        pci_config_write_dword(d.bus, d.slot, d.func, 0x44, t & ~7u);
    }
    if (!regs) regs = mmio_map(base, 0x4000);
    k_snprintf(ctrl_desc, sizeof(ctrl_desc), "%04x:%04x at %02x:%02x.%u%s", d.vendor, d.device, d.bus, d.slot,
               d.func, d.subclass == 0x01 ? " (Intel smart sound, used as plain HDA)" : "");

    /* Reset. */
    wr32(INTCTL, 0);
    wr8(CORBCTL, 0);
    wr8(RIRBCTL, 0);
    wr16(STATESTS, 0x7FFF);
    wr32(GCTL, rd32(GCTL) & ~1u);
    for (int t = 0; t < 1000 && (rd32(GCTL) & 1); t++) udelay(10);
    udelay(100);
    wr32(GCTL, rd32(GCTL) | 1);
    for (int t = 0; t < 1000 && !(rd32(GCTL) & 1); t++) udelay(10);
    if (!(rd32(GCTL) & 1)) {
        k_snprintf(how, (size_t)size, "the controller did not leave reset");
        return -1;
    }
    for (int t = 0; t < 100 && !rd16(STATESTS); t++) udelay(100);   /* codecs: within 521 us */
    udelay(1000);
    codec_mask = rd16(STATESTS) & 0x7FFF;
    gcap = rd16(GCAP);
    if (!codec_mask) {
        k_snprintf(how, (size_t)size, "no codec answered on the HDA link");
        return -1;
    }
    use_immediate = 0;
    if (setup_rings() != 0) return -1;

    int found = -1;
    for (int a = 0; a < 15 && found < 0; a++) {
        if (!(codec_mask & (1u << a))) continue;
        codec_addr = (uint8_t)a;
        if (param(0, P_VENDOR) == 0xFFFFFFFFu) {            /* rings silent: single commands */
            use_immediate = 1;
            if (param(0, P_VENDOR) == 0xFFFFFFFFu) { use_immediate = 0; continue; }
        }
        if (scan_codec((uint8_t)a) == 0) found = a;
    }
    if (found < 0) {
        k_snprintf(how, (size_t)size, "no codec with a speaker or headphone output (codecs %x)", codec_mask);
        return -1;
    }
    for (int i = 0; i < n_outs; i++) enable_path(&outs[i]);

    /* The first output stream descriptor, its buffer and descriptor list. */
    int iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF;
    if (!oss) {
        k_snprintf(how, (size_t)size, "the controller has no output streams");
        return -1;
    }
    sd = 0x80 + 0x20u * (uint32_t)iss;
    if (!ring) {
        ring = dma_alloc(RING_BYTES, 4096);
        bdl = dma_alloc(16 * BDL_ENTRIES, 128);
        if (!ring || !bdl) return -1;
    }
    for (int i = 0; i < BDL_ENTRIES; i++) {
        uint64_t a = (uint64_t)ring + (uint64_t)i * (RING_BYTES / BDL_ENTRIES);
        bdl[i * 4] = (uint32_t)a;
        bdl[i * 4 + 1] = (uint32_t)(a >> 32);
        bdl[i * 4 + 2] = RING_BYTES / BDL_ENTRIES;
        bdl[i * 4 + 3] = 0;
    }
    ready = 1;
    const char *vn = (codec_vendor >> 16) == 0x10EC ? "Realtek" : (codec_vendor >> 16) == 0x14F1 ? "Conexant" :
                     (codec_vendor >> 16) == 0x8086 ? "Intel" : (codec_vendor >> 16) == 0x1AF4 ? "QEMU" : "codec";
    k_snprintf(how, (size_t)size, "HD Audio, %s %04x, %s%s%s", vn, codec_vendor & 0xFFFF,
               device_name(node(outs[0].pin)->config), n_outs > 1 ? " + " : "",
               n_outs > 1 ? device_name(node(outs[1].pin)->config) : "");
    return 0;
}

int sound_ready(void) { return ready; }

void sound_set_volume(int percent) { volume = percent < 0 ? 0 : percent > 100 ? 100 : percent; }
int sound_get_volume(void) { return volume; }

void sound_print_info(void) {
    if (!regs) {
        char how[96];
        int r = sound_init(how, sizeof(how));
        kprintf("Sound: %s\n", how);
        if (r != 0 && !regs) return;
    }
    kprintf("Controller %s, HDA %u.%u, %u in / %u out streams, codecs at %x, %s commands\n", ctrl_desc,
            rd8(0x03), rd8(0x02), (gcap >> 8) & 0xF, (gcap >> 12) & 0xF, codec_mask,
            use_immediate ? "single" : "ring");
    if (!num_nids) return;
    kprintf("Codec %u: vendor %08x, audio function group at node %u, nodes %u-%u\n", codec_addr, codec_vendor, afg,
            first_nid, first_nid + num_nids - 1);
    static const char *tn[16] = { "DAC", "ADC", "mixer", "selector", "pin", "power", "volume", "beep" };
    if (!ready) {                                           /* no path: show the whole graph */
        for (int i = 0; i < num_nids; i++) {
            widget_t *x = &w[i];
            kprintf("  node %u: %s caps %08x, %u conn", first_nid + i, x->type < 8 && tn[x->type] ? tn[x->type] : "?",
                    x->caps, x->nconn);
            for (int k = 0; k < x->nconn; k++) kprintf(" %u", x->conn[k]);
            if (x->type == W_PIN) kprintf(", pincaps %08x config %08x", x->pincaps, x->config);
            kprintf("\n");
        }
        return;
    }
    for (int i = 0; i < num_nids; i++) {
        widget_t *x = &w[i];
        if (x->type != W_PIN || !pin_rank(x)) continue;
        kprintf("  pin %u: %s, %s, config %08x%s\n", first_nid + i, device_name(x->config),
                (x->config >> 30) == 2 ? "built in" : (x->config >> 30) == 0 ? "jack" : "jack + built in",
                x->config, x->used ? " (used)" : "");
    }
    for (int k = 0; k < n_outs; k++) {
        kprintf("  path %d:", k + 1);
        for (int i = 0; i < outs[k].len; i++) {
            widget_t *x = node(outs[k].nodes[i]);
            kprintf(" %s%u", i ? "-> " : "", outs[k].nodes[i]);
            kprintf("(%s)", x->type < 8 && tn[x->type] ? tn[x->type] : "?");
        }
        kprintf("\n");
    }
    kprintf("Volume %d%%\n", volume);
}

/* ---- Playback ----------------------------------------------------------------- */

/* Fills `frames` stereo 48 kHz frames; returns how many (0 = the end). */
typedef int (*source_fn)(int16_t *out, int frames, void *ctx);

static void stream_stop(void) {
    wr32(sd + SD_CTL, rd32(sd + SD_CTL) & ~2u);
    for (int t = 0; t < 100 && (rd32(sd + SD_CTL) & 2); t++) udelay(10);
}

static int stream_reset(void) {
    stream_stop();
    wr32(sd + SD_CTL, rd32(sd + SD_CTL) | 1);
    for (int t = 0; t < 1000 && !(rd32(sd + SD_CTL) & 1); t++) udelay(10);
    wr32(sd + SD_CTL, rd32(sd + SD_CTL) & ~1u);
    for (int t = 0; t < 1000 && (rd32(sd + SD_CTL) & 1); t++) udelay(10);
    return (rd32(sd + SD_CTL) & 1) ? -1 : 0;
}

static int play(source_fn src, void *ctx) {
    if (!ready) return -1;
    if (stream_reset() != 0) return -1;
    for (int i = 0; i < n_outs; i++) {                      /* (again: a codec may forget on idle) */
        cmd(outs[i].dac, V_SET_FORMAT, FORMAT_48K16);
        cmd(outs[i].dac, V_SET_STREAM, STREAM_TAG << 4);
    }
    wr32(sd + SD_BDPL, (uint32_t)(uint64_t)bdl);
    wr32(sd + SD_BDPU, (uint32_t)((uint64_t)bdl >> 32));
    wr32(sd + SD_CBL, RING_BYTES);
    wr16(sd + SD_LVI, BDL_ENTRIES - 1);
    wr16(sd + SD_FMT, FORMAT_48K16);
    wr32(sd + SD_CTL, (rd32(sd + SD_CTL) & ~0xF00000u) | (uint32_t)STREAM_TAG << 20);
    wr8(sd + SD_STS, 0x1C);

    /* Fill the ring, start, then keep refilling behind the play position. */
    const int frame = 4, total = RING_BYTES / frame;
    int16_t *buf = (int16_t *)ring;
    int done = 0, written = 0;                              /* frames of sound written */
    uint64_t audio_end = 0, wpos = 0, played = 0;           /* in frames, since the start */
    int got = src(buf, total, ctx);
    if (got < total) memset(buf + 2 * got, 0, (size_t)(total - got) * frame);
    written = got;
    if (got < total) { done = 1; audio_end = (uint64_t)got; }
    wpos = (uint64_t)total;
    __asm__ volatile("mfence" ::: "memory");
    wr32(sd + SD_CTL, rd32(sd + SD_CTL) | 2);               /* run */

    uint32_t last_lpib = 0, stall = 0, start = get_tick();
    int result = 0;
    while (1) {
        uint32_t lpib = rd32(sd + SD_LPIB) / frame;
        uint32_t step = (lpib + (uint32_t)total - last_lpib) % (uint32_t)total;
        last_lpib = lpib;
        played += step;
        stall = step ? 0 : stall + 1;
        if (done && played >= audio_end) break;
        if (stall > 50 && (uint32_t)(get_tick() - start) > 50) { result = -1; break; }   /* DMA not moving */
        if (keyboard_ctrl_c) {
            keyboard_ctrl_c = 0;
            result = 1;
            break;
        }
        /* Free space: everything already played, minus a margin. */
        while (wpos < played + (uint64_t)total - 1024) {
            int at = (int)(wpos % (uint64_t)total);
            int n = total - at;
            if ((uint64_t)n > played + (uint64_t)total - 1024 - wpos) n = (int)(played + (uint64_t)total - 1024 - wpos);
            if (n > 2048) n = 2048;
            int g = done ? 0 : src(buf + 2 * at, n, ctx);
            if (g < n) {
                memset(buf + 2 * (at + g), 0, (size_t)(n - g) * frame);
                if (!done) { done = 1; audio_end = wpos + (uint64_t)g; }
            }
            written += g;
            wpos += (uint64_t)n;
        }
        __asm__ volatile("mfence" ::: "memory");
        __asm__ volatile("sti; hlt");                       /* until the next tick */
    }
    stream_stop();
    (void)written;
    return result;
}

static int scale(int s) {
    s = s * volume / 100;
    return s > 32767 ? 32767 : s < -32768 ? -32768 : s;
}

/* Sine without floating point (Bhaskara I's approximation, error < 0.2%). */
static int sine(uint32_t phase) {                          /* phase: 2^32 = one turn; result +-32767 */
    uint32_t half = phase >> 17;                            /* 0..32767: position in the half wave */
    int64_t u = (int64_t)half * (32768 - half);
    int64_t v = 16 * u * 32767 / (5LL * 32768 * 32768 - 4 * u);
    return (phase & 0x80000000u) ? -(int)v : (int)v;
}

typedef struct { uint32_t phase, step, left, total, fade; } tone_t;

static int tone_source(int16_t *out, int frames, void *ctx) {
    tone_t *t = ctx;
    int n = frames < (int)t->left ? frames : (int)t->left;
    for (int i = 0; i < n; i++) {
        uint32_t remaining = t->left - (uint32_t)i, elapsed = t->total - remaining;
        int s = sine(t->phase) / 2;                         /* -6 dB */
        uint32_t edge = elapsed < remaining ? elapsed : remaining;
        if (edge < t->fade) s = (int)((int64_t)s * edge / t->fade);    /* 10 ms fade in and out: no clicks */
        s = scale(s);
        out[2 * i] = out[2 * i + 1] = (int16_t)s;
        t->phase += t->step;
    }
    t->left -= (uint32_t)n;
    return n;
}

int sound_beep(uint32_t hz, uint32_t ms) {
    if (!ready) return -1;
    if (hz < 20) hz = 20;
    if (hz > 20000) hz = 20000;
    uint32_t frames = (uint32_t)((uint64_t)ms * RATE / 1000);
    tone_t t = { 0, (uint32_t)(((uint64_t)hz << 32) / RATE), frames, frames, RATE / 100 };
    return play(tone_source, &t);
}

/* WAV: resampled to 48 kHz stereo (linear interpolation, 16.16 fixed point). */
typedef struct {
    const uint8_t *data;
    uint32_t frames, channels, bits;
    uint64_t pos, step;                                     /* 32.32 position in source frames */
} wav_t;

static int wav_sample(const wav_t *v, uint32_t f, int ch) {
    if (f >= v->frames) f = v->frames - 1;
    uint32_t c = v->channels == 1 ? 0 : (uint32_t)ch;
    if (v->bits == 8) return ((int)v->data[f * v->channels + c] - 128) << 8;
    const uint8_t *p = v->data + ((uint64_t)f * v->channels + c) * 2;
    return (int16_t)(p[0] | p[1] << 8);
}

static int wav_source(int16_t *out, int frames, void *ctx) {
    wav_t *v = ctx;
    int n = 0;
    for (; n < frames; n++) {
        uint32_t f = (uint32_t)(v->pos >> 32);
        if (f >= v->frames) break;
        uint32_t frac = (uint32_t)(v->pos >> 16) & 0xFFFF;
        for (int ch = 0; ch < 2; ch++) {
            int a = wav_sample(v, f, ch), b = wav_sample(v, f + 1, ch);
            out[2 * n + ch] = (int16_t)scale(a + (int)(((int64_t)(b - a) * frac) >> 16));
        }
        v->pos += v->step;
    }
    return n;
}

int sound_play_wav(const uint8_t *data, uint32_t size, const char **error) {
    *error = 0;
    if (!ready) { *error = "no sound device"; return -1; }
    if (size < 12 || memcmp(data, "RIFF", 4) || memcmp(data + 8, "WAVE", 4)) { *error = "not a WAV file"; return -1; }
    uint32_t rate = 0, channels = 0, bits = 0, fmt = 0, dlen = 0;
    const uint8_t *pcm = 0;
    for (uint32_t off = 12; off + 8 <= size;) {
        uint32_t len = data[off + 4] | data[off + 5] << 8 | data[off + 6] << 16 | (uint32_t)data[off + 7] << 24;
        const uint8_t *c = data + off + 8;
        if (!memcmp(data + off, "fmt ", 4) && len >= 16) {
            fmt = c[0] | c[1] << 8;
            channels = c[2] | c[3] << 8;
            rate = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
            bits = c[14] | c[15] << 8;
        } else if (!memcmp(data + off, "data", 4)) {
            pcm = c;
            dlen = len > size - off - 8 ? size - off - 8 : len;
            break;
        }
        off += 8 + len + (len & 1);
    }
    if (fmt != 1 && fmt != 0xFFFE) { *error = "only uncompressed (PCM) WAV files can be played"; return -1; }
    if (!pcm || !rate || (channels != 1 && channels != 2) || (bits != 8 && bits != 16)) {
        *error = "unsupported WAV format (needs 8 or 16 bit, mono or stereo)";
        return -1;
    }
    wav_t v = { pcm, dlen / (channels * bits / 8), channels, bits, 0, ((uint64_t)rate << 32) / RATE };
    if (!v.frames) return 0;
    return play(wav_source, &v);
}
