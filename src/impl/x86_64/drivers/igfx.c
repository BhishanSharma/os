// igfx.c - Intel integrated graphics: hardware cursor and page flipping.
//
// The firmware (UEFI GOP) has already set a mode and shows its framebuffer on
// one pipe through plane 1. We leave all of that alone and use two more
// features of the display engine:
//   - the cursor plane: the pointer is composited by the hardware, so moving
//     it is one register write and nothing is redrawn;
//   - page flips: plane 1 switches between the firmware's framebuffer and a
//     second buffer in RAM at the start of a refresh, so a frame is never
//     shown half drawn (no tearing).
// Register layout from Intel's register headers (MIT licence, as published in
// Linux's i915 driver) and the Intel Graphics PRMs; the code is our own.
#include "drivers/igfx.h"
#include "drivers/pci.h"
#include "drivers/paging.h"
#include "drivers/physmem.h"
#include "drivers/heap.h"
#include "drivers/addr.h"
#include "drivers/timer.h"
#include "lib/print.h"
#include "lib/string.h"

/* Display registers, pipe A; pipes B-D are 0x1000 apart. */
#define PIPE_STRIDE      0x1000
#define CUR_CTL          0x70080
#define CUR_BASE         0x70084
#define CUR_POS          0x70088
#define CUR_SURFLIVE     0x700AC
#define CUR_WM(l)        (0x70140 + 4 * (l))
#define CUR_WM_SAGV      0x70158
#define CUR_WM_SAGV_TRANS 0x7015C
#define CUR_WM_TRANS     0x70168
#define CUR_BUF_CFG      0x7017C
#define PIPE_MISC        0x70030            /* (Gen12: the pipe's; bits 7:5 bpc, 4 dithering) */
#define PIPE_MISC_DITHER (1u << 4)
#define PLANE_CTL        0x70180
#define PLANE_STRIDE     0x70188
#define PLANE_SIZE       0x70190
#define PLANE_SURF       0x7019C
#define PLANE_SURFLIVE   0x701AC
#define PLANE_WM(l)      (0x70240 + 4 * (l))
#define PLANE_WM_TRANS   0x70268
#define PLANE_BUF_CFG    0x7027C

#define CURSOR_MODE_64_ARGB 0x27
#define WM_EN            (1u << 31)

/* Where our buffers go in the GPU's address space (the GGTT, 4 GiB): well
 * above the firmware's framebuffer. Linear surfaces want 256 KiB alignment. */
#define GGTT_CURSOR      0xC0000000u
#define GGTT_BACK        0xC0100000u
#define GGTT_BACK2       0xC0A00000u

static volatile uint8_t *regs;          /* the register BAR, uncached */
static uint64_t gsm_phys;               /* the GGTT's page table: BAR 0 + 8 MiB */
static int pipe = -1;
static uint32_t surf0;                  /* the firmware framebuffer's GGTT address */
static uint32_t surf_low;               /* low bits of PLANE_SURF to keep */
static uint32_t width, height, pitch;
static int ok, cursor_on, cursor_hx, cursor_hy;
static uint32_t *cursor_va, *back_va, *back2_va;
static uint32_t flipped_to;
static char status[160] = "no Intel graphics";

static uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(regs + r + (uint32_t)pipe * PIPE_STRIDE); }
static void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(regs + r + (uint32_t)pipe * PIPE_STRIDE) = v; }
static uint32_t rd_pipe(int p, uint32_t r) { return *(volatile uint32_t *)(regs + r + (uint32_t)p * PIPE_STRIDE); }

const char *igfx_status(void) { return status; }

/* Gen12/13 display: Tiger Lake, Rocket Lake, Alder Lake, Raptor Lake. */
static int supported(uint16_t dev) {
    return (dev >= 0x9A40 && dev <= 0x9AFF) || (dev >= 0x4C80 && dev <= 0x4C8F) ||
           (dev >= 0x4680 && dev <= 0x46DF) || (dev >= 0x4620 && dev <= 0x462F) ||
           (dev >= 0xA780 && dev <= 0xA7AF) || dev == 0xA720 || dev == 0xA721;
}

/* `n` pages of RAM, mapped for the CPU (write-combining) and at `ggtt` for
 * the display engine. -> the CPU address, 0 if out of memory. */
static uint32_t *gpu_buffer(uint32_t ggtt, uint32_t n) {
    uint64_t *pages = kmalloc((uint64_t)n * 8);
    if (!pages) return 0;
    for (uint32_t i = 0; i < n; i++) {
        pages[i] = page_alloc();
        if (!pages[i]) return 0;
    }
    __asm__ volatile("wbinvd" ::: "memory");            /* no stale cache lines over them */
    volatile uint64_t *pte = mmio_map(gsm_phys + (uint64_t)(ggtt >> 12) * 8, (uint64_t)n * 8);
    for (uint32_t i = 0; i < n; i++) pte[i] = pages[i] | 1;   /* present */
    (void)pte[n - 1];                                   /* the writes are done */
    uint32_t *va = paging_map_pages_wc(pages, n);
    kfree(pages);
    return va;
}

/* ---- The engines (step 1: look, do not touch) -------------------------------
 * The blitter (BCS) copies and fills memory for the CPU; the render engine
 * runs shaders. Before driving them: what state the firmware left them in,
 * into the system log. Only reads, and a forcewake (keeps the GT powered while
 * we read, as i915 does). */
static uint32_t mm(uint32_t r) { return *(volatile uint32_t *)(regs + r); }
static void mm_wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(regs + r) = v; }

#define FORCEWAKE_GT     0xA188
#define FORCEWAKE_GT_ACK 0x130044
#define GUC_STATUS       0xC000

static void engine_log(const char *name, uint32_t base) {
    char line[200];
    k_snprintf(line, sizeof(line),
               "igfx: %s: head %x tail %x start %x ctl %x acthd %x mi_mode %x ring_mode %x execlist %x:%x hws %x\n",
               name, mm(base + 0x34), mm(base + 0x30), mm(base + 0x38), mm(base + 0x3C), mm(base + 0x74),
               mm(base + 0x9C), mm(base + 0x29C), mm(base + 0x234), mm(base + 0x238), mm(base + 0x80));
    print_log(line);
}

void igfx_engines_probe(void) {
    if (!regs) return;
    char line[160];
    mm_wr(FORCEWAKE_GT, 0x00010001);                    /* (mask << 16) | wake */
    uint64_t end = timer_ms() + 50;
    while (!(mm(FORCEWAKE_GT_ACK) & 1) && timer_ms() < end) { }
    k_snprintf(line, sizeof(line), "igfx: forcewake %s, GuC status %x, GGTT at %x (BAR 0 + 8 MiB)\n",
               (mm(FORCEWAKE_GT_ACK) & 1) ? "acked" : "NOT acked", mm(GUC_STATUS), (uint32_t)gsm_phys);
    print_log(line);
    engine_log("render (RCS)", 0x2000);
    engine_log("blitter (BCS)", 0x22000);
    mm_wr(FORCEWAKE_GT, 0x00010000);                    /* let it sleep again */
}

int igfx_init(const fb_info_t *fb) {
    uint32_t id = pci_config_read_dword(0, 2, 0, 0);
    uint16_t vendor = (uint16_t)id, dev = (uint16_t)(id >> 16);
    if (vendor != 0x8086) return -1;
    k_snprintf(status, sizeof(status), "Intel %04x: not a Gen12/13 display (software pointer)", dev);
    if (!supported(dev)) return -1;
    uint64_t bar = (pci_config_read_dword(0, 2, 0, 0x10) & ~0xFULL) |
                   ((uint64_t)pci_config_read_dword(0, 2, 0, 0x14) << 32);
    if (!bar) return -1;
    regs = mmio_map(bar, 4 << 20);
    gsm_phys = bar + (8 << 20);

    /* The pipe whose plane 1 is on and shows a linear surface of our size. */
    for (int p = 0; p < 4 && pipe < 0; p++) {
        uint32_t ctl = rd_pipe(p, PLANE_CTL);
        if (!(ctl & (1u << 31))) continue;
        uint32_t size = rd_pipe(p, PLANE_SIZE), stride = rd_pipe(p, PLANE_STRIDE) & 0xFFF;
        uint32_t w = (size & 0x1FFF) + 1, h = ((size >> 16) & 0x1FFF) + 1;
        if (((ctl >> 10) & 7) != 0) continue;            /* tiled: not the firmware's */
        if (w != fb->width || h != fb->height || stride * 64 != fb->pitch) continue;
        pipe = p;
    }
    if (pipe < 0) {
        k_snprintf(status, sizeof(status), "Intel %04x: no plane shows the %ux%u framebuffer (software pointer)",
                   dev, fb->width, fb->height);
        return -1;
    }
    /* The pipe's colour depth: a 6-bit panel's pipe (6 bpc) without dithering
     * shows a gradient as bands four levels wide. i915 turns dithering on
     * for it; the firmware may not have. */
    uint32_t misc = rd(PIPE_MISC), bpc = (misc >> 5) & 7;
    char ml[160];
    k_snprintf(ml, sizeof(ml), "igfx: pipe %c: misc %x (%s bpc, dithering %s), plane ctl %x\n", 'A' + pipe, misc,
               bpc == 0 ? "8" : bpc == 1 ? "10" : bpc == 2 ? "6" : bpc == 3 ? "12" : "?",
               (misc & PIPE_MISC_DITHER) ? "on" : "off", rd(PLANE_CTL));
    print_log(ml);
    if (bpc == 2 && (misc & (PIPE_MISC_DITHER | 3u << 2)) != (PIPE_MISC_DITHER | 1u << 2)) {
        /* spatio-temporal (1): the pattern also changes from frame to frame,
         * which a 6-bit panel needs for a dark gradient (spatial alone left bands) */
        wr(PIPE_MISC, (misc & ~(3u << 2)) | PIPE_MISC_DITHER | 1u << 2);
        k_snprintf(ml, sizeof(ml), "igfx: pipe %c: dithering on (misc %x)\n", 'A' + pipe, rd(PIPE_MISC));
        print_log(ml);
    }
    uint32_t s = rd(PLANE_SURF);
    surf0 = s & ~0xFFFu;
    surf_low = s & 0xFFFu;
    width = fb->width;
    height = fb->height;
    pitch = fb->pitch;
    flipped_to = surf0;
    ok = 1;
    k_snprintf(status, sizeof(status), "Intel %04x, pipe %c %ux%u: hardware cursor, page flipping",
               dev, 'A' + pipe, width, height);
    return 0;
}

/* Display buffer (DDB) blocks and watermarks for the cursor, if the firmware
 * gave it none: the last 32 blocks of plane 1's share. */
static int cursor_ddb(void) {
    uint32_t cbuf = rd(CUR_BUF_CFG);
    uint32_t cstart = cbuf & 0xFFF, cend = (cbuf >> 16) & 0xFFF;
    if (cend > cstart) return (int)(cend - cstart + 1);
    uint32_t pbuf = rd(PLANE_BUF_CFG);
    uint32_t pstart = pbuf & 0x1FFF, pend = (pbuf >> 16) & 0x1FFF;
    uint32_t need = 32, most = 0;
    for (int l = 0; l < 8; l++) {
        uint32_t wm = rd(PLANE_WM(l));
        if ((wm & WM_EN) && (wm & 0x7FF) > most) most = wm & 0x7FF;
    }
    uint32_t t = rd(PLANE_WM_TRANS);
    if ((t & WM_EN) && (t & 0x7FF) > most) most = t & 0x7FF;
    if (pend < pstart + need || pend - pstart + 1 - need < most + 8) {
        need = 16;
        if (pend < pstart + need || pend - pstart + 1 - need < most + 8) return 0;
    }
    uint32_t newend = pend - need;
    wr(PLANE_BUF_CFG, (newend << 16) | pstart);
    wr(PLANE_SURF, rd(PLANE_SURF));                     /* arms plane 1's update */
    sleep(40);                                          /* two refreshes */
    wr(CUR_BUF_CFG, (pend << 16) | (newend + 1));
    return (int)need;
}

int igfx_cursor_set(const uint32_t *argb, int w, int h, int hx, int hy, int x, int y) {
    if (!ok || w > 64 || h > 64) return -1;
    if (!cursor_va) {
        cursor_va = gpu_buffer(GGTT_CURSOR, 4);         /* 64 x 64 x 4 bytes */
        if (!cursor_va) return -1;
    }
    for (int j = 0; j < 64; j++)                        /* premultiplied, as the cursor plane blends */
        for (int i = 0; i < 64; i++) {
            uint32_t p = (i < w && j < h) ? argb[j * w + i] : 0, a = p >> 24;
            uint32_t r = ((p >> 16) & 255) * a / 255, g = ((p >> 8) & 255) * a / 255, b = (p & 255) * a / 255;
            cursor_va[j * 64 + i] = a << 24 | r << 16 | g << 8 | b;
        }
    __asm__ volatile("sfence" ::: "memory");
    cursor_hx = hx;
    cursor_hy = hy;
    if (!cursor_on) {
        int blocks = cursor_ddb();
        if (blocks < 8) {
            k_snprintf(status + strlen(status), sizeof(status) - strlen(status), "; no buffer for the cursor");
            return -1;
        }
        uint32_t wm = WM_EN | (4u << 14) | (uint32_t)(blocks - 2);
        wr(CUR_WM(0), wm);
        for (int l = 1; l < 8; l++) wr(CUR_WM(l), 0);
        wr(CUR_WM_TRANS, wm);
        wr(CUR_WM_SAGV, wm);
        wr(CUR_WM_SAGV_TRANS, wm);
        wr(CUR_CTL, CURSOR_MODE_64_ARGB);
    }
    cursor_on = 1;
    igfx_cursor_move(x, y);
    sleep(40);
    if ((rd(CUR_SURFLIVE) & ~0xFFFu) != GGTT_CURSOR) {  /* the hardware did not take it */
        igfx_cursor_off();
        k_snprintf(status + strlen(status), sizeof(status) - strlen(status), "; cursor not taken");
        return -1;
    }
    return 0;
}

void igfx_cursor_move(int x, int y) {
    if (!cursor_on) return;
    x -= cursor_hx;
    y -= cursor_hy;
    uint32_t pos = (x < 0 ? (1u << 15) | (uint32_t)(-x) : (uint32_t)x) |
                   ((y < 0 ? (1u << 15) | (uint32_t)(-y) : (uint32_t)y) << 16);
    wr(CUR_POS, pos);
    wr(CUR_BASE, GGTT_CURSOR);                          /* arms the update */
}

void igfx_cursor_off(void) {
    if (!ok || !cursor_on) return;
    wr(CUR_CTL, 0);
    wr(CUR_BASE, GGTT_CURSOR);
    cursor_on = 0;
}

uint32_t *igfx_buffer(int i) {
    if (!ok || i < 1 || i > 2) return 0;
    uint32_t **va = i == 1 ? &back_va : &back2_va;
    if (!*va) *va = gpu_buffer(i == 1 ? GGTT_BACK : GGTT_BACK2, (pitch * height + 4095) / 4096);
    return *va;
}

uint32_t *igfx_back_buffer(void) { return igfx_buffer(1); }

void igfx_flip(int buffer) {
    if (!ok) return;
    flipped_to = buffer == 2 ? GGTT_BACK2 : buffer ? GGTT_BACK : surf0;
    __asm__ volatile("sfence" ::: "memory");
    wr(PLANE_SURF, flipped_to | surf_low);              /* taken at the next refresh */
}

int igfx_flip_done(void) {
    return !ok || (rd(PLANE_SURFLIVE) & ~0xFFFu) == flipped_to;
}

/* ---- The blitter (step 2: one command) -------------------------------------
 * Gen11+ engines take work only through execlists: a context (a page for the
 * engine's status, then its registers as MI_LOAD_REGISTER_IMM lists) whose
 * ring buffer holds the commands, submitted by writing its descriptor to the
 * engine's submit queue. The first command: store 0x600DF00D into memory the
 * CPU watches. Layout of the context's first registers (ring head, tail,
 * start, control) as in i915's gen12_xcs_offsets. Everything here only
 * reports: if the engine does not answer, the desktop does not use it. */
#define BCS              0x22000
#define RING_TAIL        0x30
#define RING_HEAD        0x34
#define RING_START       0x38
#define RING_CTL         0x3C
#define RING_HWS_PGA     0x80
#define RING_MI_MODE     0x9C
#define RING_CTX_CONTROL 0x244
#define RING_MODE        0x29C
#define EXECLIST_STATUS  0x234
#define EXECLIST_SQ_LO   0x510
#define EXECLIST_SQ_HI   0x514
#define EXECLIST_CONTROL 0x550
#define MASKED_ON(b)     (((b) << 16) | (b))
#define MASKED_OFF(b)    ((b) << 16)

#define MI_NOOP          0
#define MI_LRI(n)        ((0x22u << 23) | (1u << 12) | (2u * (n) - 1))   /* force posted */
#define MI_STORE_GGTT    ((0x20u << 23) | (1u << 22) | 2)                 /* 64-bit address, GGTT */

#define GGTT_ENGINE      0xC2000000u            /* the status page, the context, the ring */
#define CTX_PAGES        22                     /* PPHWSP + registers (i915: 22 for Gen12 xcs) */
#define RING_PAGES       4

static int blitter_ok;
static char blitter_status[120] = "not tried";
static uint32_t *ctx_regs, *ctx_ring, *ctx_data;  /* the context's registers, ring, the number it writes */
static uint32_t ctx_data_g, ctx_desc_lo, ctx_desc_hi;
static uint32_t ring_tail;

const char *igfx_blitter_status(void) { return blitter_status; }

int igfx_blitter_test(void) {
    if (!ok) return -1;
    uint32_t hws_g = GGTT_ENGINE, ctx_g = hws_g + 4096, ring_g = ctx_g + CTX_PAGES * 4096,
             data_g = ring_g + RING_PAGES * 4096;
    uint32_t *hws = gpu_buffer(hws_g, 1), *ctx = gpu_buffer(ctx_g, CTX_PAGES),
             *ring = gpu_buffer(ring_g, RING_PAGES), *data = gpu_buffer(data_g, 1);
    ctx_regs = ctx + 1024;
    ctx_ring = ring;
    ctx_data = data;
    ctx_data_g = data_g;
    if (!hws || !ctx || !ring || !data) {
        k_snprintf(blitter_status, sizeof(blitter_status), "out of memory");
        return -1;
    }
    memset(hws, 0, 4096);
    memset(ctx, 0, CTX_PAGES * 4096);
    memset(ring, 0, RING_PAGES * 4096);
    data[0] = 0;

    /* The ring: one store, then padding to an even number of dwords. */
    int n = 0;
    ring[n++] = MI_STORE_GGTT;
    ring[n++] = data_g;
    ring[n++] = 0;
    ring[n++] = 0x600DF00D;
    ring[n++] = MI_NOOP;
    ring[n++] = MI_NOOP;
    uint32_t tail = (uint32_t)n * 4;

    /* The context's registers (page 1; page 0 is the per-process status page). */
    uint32_t *r = ctx + 1024;
    r[0] = MI_NOOP;
    r[1] = MI_LRI(13);
    r[2] = BCS + RING_CTX_CONTROL;
    r[3] = MASKED_ON(1u << 0) |                       /* engine context restore inhibit (first run) */
           MASKED_ON(1u << 3);                        /* (inhibit synchronous context switch) */
    r[4] = BCS + RING_HEAD;  r[5] = 0;
    r[6] = BCS + RING_TAIL;  r[7] = tail;
    r[8] = BCS + RING_START; r[9] = ring_g;
    r[10] = BCS + RING_CTL;  r[11] = ((RING_PAGES - 1) * 4096u) | 1;   /* size - 4 KiB, valid */
    for (int i = 0; i < 9; i++) {                     /* batch buffer state etc: zero */
        static const uint16_t more[9] = { 0x168, 0x140, 0x110, 0x1C0, 0x1C4, 0x1C8, 0x180, 0x2B4, 0 };
        if (!more[i]) break;
        r[12 + 2 * i] = BCS + more[i];
        r[13 + 2 * i] = 0;
    }
    r[33] = MI_LRI(9);                                /* the timestamp, the page directory pointers */
    static const uint16_t pdp[9] = { 0x3A8, 0x28C, 0x288, 0x284, 0x280, 0x27C, 0x278, 0x274, 0x270 };
    for (int i = 0; i < 9; i++) {
        r[34 + 2 * i] = BCS + pdp[i];
        r[35 + 2 * i] = 0;                            /* PDP0 (49 high, 51 low): igfx_copy */
    }
    r[52] = (0x0Au << 23);                            /* MI_BATCH_BUFFER_END: the list ends */
    __asm__ volatile("sfence" ::: "memory");

    mm_wr(FORCEWAKE_GT, 0x00010001);
    uint64_t end = timer_ms() + 50;
    while (!(mm(FORCEWAKE_GT_ACK) & 1) && timer_ms() < end) { }

    /* Execlists on, the engine's status page, the ring not stopped. */
    mm_wr(BCS + RING_HWS_PGA, hws_g);
    mm_wr(BCS + RING_MODE, MASKED_ON(1u << 3));       /* GEN11_GFX_DISABLE_LEGACY_MODE */
    mm_wr(BCS + RING_MI_MODE, MASKED_OFF(1u << 8));   /* STOP_RING off */
    (void)mm(BCS + RING_MODE);

    /* The descriptor: valid, legacy 32-bit addressing, privileged, the context's GGTT address. */
    uint32_t lo = ctx_g | (1u << 8) | (1u << 3) | (1u << 1) | (1u << 0);   /* (bit 1: reload the page tables) */
    uint32_t hi = (1u << (37 - 32)) | (3u << (61 - 32));   /* software context id 1, class: copy */
    mm_wr(BCS + EXECLIST_SQ_LO, lo);
    mm_wr(BCS + EXECLIST_SQ_HI, hi);
    mm_wr(BCS + EXECLIST_CONTROL, 1);                 /* load */
    ctx_desc_lo = lo;
    ctx_desc_hi = hi;
    ring_tail = tail;

    end = timer_ms() + 100;
    while (*(volatile uint32_t *)data != 0x600DF00D && timer_ms() < end) __asm__ volatile("pause");
    uint32_t got = *(volatile uint32_t *)data, ms = (uint32_t)(timer_ms() + 100 - end);
    char line[220];
    k_snprintf(line, sizeof(line),
               "igfx: blitter test: %s (wrote %x) in %u ms; head %x tail %x ctl %x execlist %x:%x mode %x\n",
               got == 0x600DF00D ? "ok" : "no answer", got, ms, mm(BCS + RING_HEAD), mm(BCS + RING_TAIL),
               mm(BCS + RING_CTL), mm(BCS + EXECLIST_STATUS), mm(BCS + EXECLIST_STATUS + 4), mm(BCS + RING_MODE));
    print_log(line);
    blitter_ok = got == 0x600DF00D;
    mm_wr(FORCEWAKE_GT, 0x00010000);                  /* (each batch wakes it again) */
    k_snprintf(blitter_status, sizeof(blitter_status), "%s", blitter_ok ? "blitter answers" : "blitter: no answer");
    return blitter_ok ? 0 : -1;
}

/* ---- The render engine (GPU driver stage 1: one command) --------------------
 * The render engine (RCS) runs shaders: blur, transparency, scaling, the
 * things the blitter cannot do. Brought up like the blitter (execlists, a
 * context whose image is only its ring registers, restore inhibited), with
 * what differs: its registers at 0x2000, its own power well (the render
 * forcewake domain), a bigger context image (the engine saves its whole 3D
 * state there when it switches out), engine class 0. The test stores
 * 0x600DF00D; nothing uses the engine yet. */
#define RCS              0x2000
#define GGTT_RENDER      0xC4000000u            /* its status page, context, ring, the word it writes */
#define RCS_CTX_PAGES    96                     /* (Gen12 render: about 80 pages; more is harmless) */
#define FORCEWAKE_RENDER 0xA278
#define FORCEWAKE_RENDER_ACK 0x0D84

static char render_status[120] = "not tried";
static int render_ok;
static uint32_t *rcs_regs, *rcs_ring, *rcs_data;  /* kept for the shaders (stage 2) */
static uint32_t rcs_data_g, rcs_desc_lo, rcs_desc_hi, rcs_tail;

const char *igfx_render_status(void) { return render_status; }

int igfx_render_test(void) {
    if (!ok) return -1;
    uint32_t hws_g = GGTT_RENDER, ctx_g = hws_g + 4096, ring_g = ctx_g + RCS_CTX_PAGES * 4096,
             data_g = ring_g + RING_PAGES * 4096;
    uint32_t *hws = gpu_buffer(hws_g, 1), *ctx = gpu_buffer(ctx_g, RCS_CTX_PAGES),
             *ring = gpu_buffer(ring_g, RING_PAGES), *data = gpu_buffer(data_g, 1);
    if (!hws || !ctx || !ring || !data) {
        k_snprintf(render_status, sizeof(render_status), "out of memory");
        return -1;
    }
    memset(hws, 0, 4096);
    memset(ctx, 0, RCS_CTX_PAGES * 4096);
    memset(ring, 0, RING_PAGES * 4096);
    data[0] = 0;

    int n = 0;
    ring[n++] = MI_STORE_GGTT;
    ring[n++] = data_g;
    ring[n++] = 0;
    ring[n++] = 0x600DF00D;
    ring[n++] = MI_NOOP;
    ring[n++] = MI_NOOP;
    uint32_t tail = (uint32_t)n * 4;

    /* The ring registers, as for the blitter (i915's gen12_rcs_offsets
     * starts the same way), then the page directory pointers. */
    uint32_t *r = ctx + 1024;
    r[0] = MI_NOOP;
    r[1] = MI_LRI(13);
    r[2] = RCS + RING_CTX_CONTROL;
    r[3] = MASKED_ON(1u << 0) | MASKED_ON(1u << 3);   /* restore inhibit (first run), no sync switch */
    r[4] = RCS + RING_HEAD;  r[5] = 0;
    r[6] = RCS + RING_TAIL;  r[7] = tail;
    r[8] = RCS + RING_START; r[9] = ring_g;
    r[10] = RCS + RING_CTL;  r[11] = ((RING_PAGES - 1) * 4096u) | 1;
    static const uint16_t more[8] = { 0x168, 0x140, 0x110, 0x1C0, 0x1C4, 0x1C8, 0x180, 0x2B4 };
    for (int i = 0; i < 8; i++) {
        r[12 + 2 * i] = RCS + more[i];
        r[13 + 2 * i] = 0;
    }
    r[28] = MI_NOOP;                                  /* (pads the list to where the next starts) */
    r[29] = MI_NOOP;
    r[30] = MI_NOOP;
    r[31] = MI_NOOP;
    r[32] = MI_NOOP;
    r[33] = MI_LRI(9);
    static const uint16_t pdp[9] = { 0x3A8, 0x28C, 0x288, 0x284, 0x280, 0x27C, 0x278, 0x274, 0x270 };
    for (int i = 0; i < 9; i++) {
        r[34 + 2 * i] = RCS + pdp[i];
        r[35 + 2 * i] = 0;
    }
    r[52] = (0x0Au << 23);                            /* MI_BATCH_BUFFER_END */
    __asm__ volatile("sfence" ::: "memory");

    mm_wr(FORCEWAKE_RENDER, 0x00010001);
    uint64_t end = timer_ms() + 50;
    while (!(mm(FORCEWAKE_RENDER_ACK) & 1) && timer_ms() < end) { }
    int awake = mm(FORCEWAKE_RENDER_ACK) & 1;

    mm_wr(RCS + RING_HWS_PGA, hws_g);
    mm_wr(RCS + RING_MODE, MASKED_ON(1u << 3));       /* GEN11_GFX_DISABLE_LEGACY_MODE */
    mm_wr(RCS + RING_MI_MODE, MASKED_OFF(1u << 8));   /* STOP_RING off */
    (void)mm(RCS + RING_MODE);

    uint32_t lo = ctx_g | (1u << 8) | (1u << 3) | (1u << 1) | (1u << 0);
    uint32_t hi = (2u << (37 - 32)) | (0u << (61 - 32));   /* software context id 2, class: render */
    rcs_regs = r;
    rcs_ring = ring;
    rcs_data = data;
    rcs_data_g = data_g;
    rcs_desc_lo = lo;
    rcs_desc_hi = hi;
    rcs_tail = tail;
    mm_wr(RCS + EXECLIST_SQ_LO, lo);
    mm_wr(RCS + EXECLIST_SQ_HI, hi);
    mm_wr(RCS + EXECLIST_CONTROL, 1);

    end = timer_ms() + 100;
    while (*(volatile uint32_t *)data != 0x600DF00D && timer_ms() < end) __asm__ volatile("pause");
    uint32_t got = *(volatile uint32_t *)data, ms = (uint32_t)(timer_ms() + 100 - end);
    char line[240];
    k_snprintf(line, sizeof(line),
               "igfx: render test: %s (wrote %x) in %u ms; forcewake %s; head %x tail %x ctl %x execlist %x:%x mode %x\n",
               got == 0x600DF00D ? "ok" : "no answer", got, ms, awake ? "acked" : "NOT acked",
               mm(RCS + RING_HEAD), mm(RCS + RING_TAIL), mm(RCS + RING_CTL),
               mm(RCS + EXECLIST_STATUS), mm(RCS + EXECLIST_STATUS + 4), mm(RCS + RING_MODE));
    print_log(line);
    render_ok = got == 0x600DF00D;
    mm_wr(FORCEWAKE_RENDER, 0x00010000);
    k_snprintf(render_status, sizeof(render_status), "%s", render_ok ? "render engine answers" : "render engine: no answer");
    return render_ok ? 0 : -1;
}

/* ---- The blitter (step 3: copying frames) ----------------------------------
 * The desktop's frame is drawn into the shadow (RAM); presenting copies the
 * changed rectangles into the buffer the screen shows next. The blitter does
 * that copy (XY_FAST_COPY_BLT) instead of the CPU. Its commands address
 * memory through the context's own page tables (PPGTT, legacy 32-bit: four
 * page directory pointers), not the GGTT; ours map the shadow at VA_SHADOW
 * and the two screen buffers at VA_BUF1 and VA_BUF2 (both in RAM: the
 * firmware's framebuffer is in stolen memory, which the blitter may not
 * reach). A command that does not finish in 50 ms turns the blitter off and
 * the CPU copies again. */
#define VA_SHADOW        0x10000000u
#define VA_BUF1          0x20000000u
#define VA_BUF2          0x30000000u
#define XY_FAST_COPY     ((2u << 29) | (0x42u << 22) | (10 - 2))
#define FAST_COPY_32BPP  (3u << 24)
#define MI_FLUSH_DW      ((0x26u << 23) | 2)
#define PTE_RW           3u                     /* present, writable */

static uint64_t *ppgtt_pd;                      /* the one directory (VA 0 - 1 GiB) */
static const uint32_t *mapped_shadow;
static uint32_t seqno, ring_bytes = RING_PAGES * 4096;
static uint32_t src_off, src_pitch;              /* where the source starts in VA_SHADOW, its row length in bytes */
static int tables_changed;                       /* the GPU may hold old translations: invalidate first */

/* The physical page behind a kernel address (4 KiB or inside a 2 MiB page). */
static uint64_t phys_of(uint64_t va) {
    uint64_t e = paging_get_entry(va);
    if (!(e & PAGE_PRESENT)) return 0;
    if (e & PAGE_SIZE_2MB) return (e & 0x000FFFFFFFE00000ULL) + (va & 0x1FF000);
    return e & 0x000FFFFFFFFFF000ULL;
}

static int ppgtt_map(uint32_t va, uint64_t phys) {
    uint64_t *pde = &ppgtt_pd[(va >> 21) & 511];
    if (!*pde) {
        uint64_t pt = page_alloc();
        if (!pt) return -1;
        memset(phys_to_virt(pt), 0, 4096);
        *pde = pt | PTE_RW;
    }
    uint64_t *pt = phys_to_virt(*pde & ~0xFFFULL);
    pt[(va >> 12) & 511] = phys | PTE_RW;
    return 0;
}

/* [cpu, cpu + bytes) from `va` on, which is then where `cpu`'s page starts:
 * the data itself is at va + (cpu & 0xFFF). */
static int ppgtt_map_range(uint32_t va, const void *cpu, uint64_t bytes) {
    uint64_t first = (uint64_t)cpu & ~0xFFFULL;
    bytes += (uint64_t)cpu & 0xFFF;
    for (uint64_t off = 0; off < bytes; off += 4096) {
        uint64_t p = phys_of(first + off);
        if (!p || ppgtt_map(va + (uint32_t)off, p)) return -1;
    }
    return 0;
}

int igfx_blitter_ready(void) { return blitter_ok; }

static void blitter_off(const char *why) {
    blitter_ok = 0;
    k_snprintf(blitter_status, sizeof(blitter_status), "blitter off: %s", why);
    char line[200];
    k_snprintf(line, sizeof(line), "igfx: blitter off (%s); head %x tail %x execlist %x:%x\n", why,
               mm(BCS + RING_HEAD), mm(BCS + RING_TAIL), mm(BCS + EXECLIST_STATUS), mm(BCS + EXECLIST_STATUS + 4));
    print_log(line);
}

/* The page tables for the copies (once): the screen buffers now, the shadow
 * when it is first seen. */
static int blitter_tables(const uint32_t *shadow) {
    uint64_t bytes = (uint64_t)pitch * height;
    if (!ppgtt_pd) {
        uint64_t pd = page_alloc();
        uint32_t *b2 = igfx_buffer(2), *b1 = igfx_buffer(1);
        if (!pd || !b1 || !b2) return -1;
        ppgtt_pd = phys_to_virt(pd);
        memset(ppgtt_pd, 0, 4096);
        if (ppgtt_map_range(VA_BUF1, b1, bytes) || ppgtt_map_range(VA_BUF2, b2, bytes)) return -1;
        uint32_t *r = ctx_regs;                       /* PDP0 (low, high) in the context */
        r[49] = (uint32_t)(pd >> 32);
        r[51] = (uint32_t)pd;
    }
    if (shadow && shadow != mapped_shadow) {   /* (0: the buffers only) */
        if (ppgtt_map_range(VA_SHADOW, shadow, (uint64_t)width * height * 4)) return -1;
        mapped_shadow = shadow;
        src_off = (uint32_t)((uint64_t)shadow & 0xFFF);   /* (the heap's block need not start a page) */
        src_pitch = width * 4;
        tables_changed = 1;
        __asm__ volatile("wbinvd" ::: "memory");      /* the tables out of the CPU's caches */
    }
    return 0;
}

/* The source is a program's frame instead (its physical pages, `pages[0]`
 * holding its first pixel at `offset`): the blitter reads it where the
 * program drew it, and the CPU copies nothing. `token` is the program's
 * address of it. Under the kernel lock. -> 0 */
int igfx_copy_source(const void *token, const uint64_t *pages, int n, uint32_t offset, uint32_t pitch_bytes) {
    if (!blitter_ok || blitter_tables(0) < 0) return -1;
    for (int i = 0; i < n; i++)
        if (ppgtt_map(VA_SHADOW + (uint32_t)i * 4096, pages[i])) return -1;
    mapped_shadow = token;
    src_off = offset;
    src_pitch = pitch_bytes;
    tables_changed = 1;
    __asm__ volatile("wbinvd" ::: "memory");
    return 0;
}

/* Under the kernel lock (it allocates): the tables for this shadow. -> 0 ready. */
int igfx_copy_prepare(const uint32_t *shadow) {
    if (!blitter_ok) return -1;
    if (blitter_tables(shadow)) {
        blitter_off("no memory for its page tables");
        return -1;
    }
    return 0;
}

/* ---- The blitter (step 4: batches, sleeping, window contents) ---------------
 * One batch a frame: window contents into the desktop's frame (copies the
 * desktop asked for, GFX_COPY), then the frame's changed rectangles into the
 * screen buffer, a flush, and the frame's number written to memory. The
 * kernel does not wait for it: fbcon flips when the number shows up.
 * Forcewake (the GT kept out of its sleep state, RC6) is held from the
 * submission until the batch is done, so the GPU sleeps between frames.
 * Programs' memory (the windows) is mapped into the blitter's tables in
 * 16 MiB slots below VA_SHADOW, checked against the program's pages on each
 * use. */
#define SLOTS            15
#define SLOT_VA(i)       (0x01000000u * (uint32_t)((i) + 1))
#define SLOT_MAX         0x01000000u

static struct { uint64_t root, va, bytes; uint32_t used; } slots[SLOTS];
static uint32_t slot_clock;
static uint32_t *bc, *bstart;                       /* the batch being written into the ring */
static int fw_held;

static void fw_get(void) {
    if (fw_held) return;
    mm_wr(FORCEWAKE_GT, 0x00010001);
    uint64_t end = timer_ms() + 2;
    while (!(mm(FORCEWAKE_GT_ACK) & 1) && timer_ms() <= end) __asm__ volatile("pause");
    fw_held = 1;
}

static void fw_put(void) {
    if (!fw_held) return;
    mm_wr(FORCEWAKE_GT, 0x00010000);
    fw_held = 0;
}

/* A PPGTT entry changed: out of the CPU's caches (the GPU reads the tables
 * from memory), and the GPU's own copies of them are invalidated next batch. */
static void pte_set(uint32_t va, uint64_t phys) {
    uint64_t *pde = &ppgtt_pd[(va >> 21) & 511];
    if (!*pde) {                                      /* a new table: all of it out of the caches */
        if (ppgtt_map(va, phys) == 0) __asm__ volatile("wbinvd" ::: "memory");
        tables_changed = 1;
        return;
    }
    uint64_t *pte = (uint64_t *)phys_to_virt(*pde & ~0xFFFULL) + ((va >> 12) & 511);
    if (*pte == (phys | PTE_RW)) return;
    *pte = phys | PTE_RW;
    __asm__ volatile("clflush (%0)" :: "r"(pte) : "memory");
    tables_changed = 1;
}

uint32_t igfx_map_user(uint64_t root, uint64_t va, uint64_t bytes) {
    if (!blitter_ok || !ppgtt_pd || !bytes) return 0;
    uint64_t first = va & ~0xFFFULL, span = (va & 0xFFF) + bytes;
    if (span > SLOT_MAX) return 0;
    int k = -1;
    for (int i = 0; i < SLOTS; i++)
        if (slots[i].root == root && slots[i].va == first && slots[i].bytes >= span) k = i;
    if (k < 0) {                                       /* the slot used longest ago */
        k = 0;
        for (int i = 1; i < SLOTS; i++)
            if (slots[i].used < slots[k].used) k = i;
        slots[k].root = root;
        slots[k].va = first;
        slots[k].bytes = span;
    }
    slots[k].used = ++slot_clock;
    for (uint64_t off = 0; off < span; off += 4096) {  /* (the program may have new pages there) */
        uint64_t e = paging_get_user_entry(root, first + off);
        if (!(e & 1)) {
            slots[k].root = 0;
            return 0;
        }
        pte_set(SLOT_VA(k) + (uint32_t)off, e & 0x000FFFFFFFFFF000ULL);
    }
    __asm__ volatile("mfence" ::: "memory");
    return SLOT_VA(k) + (uint32_t)(va & 0xFFF);
}

static int batch_begin(uint32_t dwords) {
    uint32_t need = (dwords + 4 + 4 + 4 + 2) * 4;
    if (need > ring_bytes / 2) return -1;
    if (ring_tail + need > ring_bytes) {              /* wrap: no-ops to the end */
        while (ring_tail < ring_bytes) ctx_ring[ring_tail / 4] = MI_NOOP, ring_tail += 4;
        ring_tail = 0;
    }
    bc = bstart = ctx_ring + ring_tail / 4;
    if (tables_changed) {                             /* forget old translations (needs a post-sync write) */
        *bc++ = MI_FLUSH_DW | (1u << 18) | (1u << 14);
        *bc++ = (ctx_data_g + 4) | (1u << 2);         /* (GGTT) */
        *bc++ = 0;
        *bc++ = 0;
        tables_changed = 0;
    }
    return 0;
}

static void batch_copy(uint32_t dst, uint32_t dst_pitch, int x0, int y0, int x1, int y1,
                       uint32_t src, uint32_t src_pitch, int sx, int sy) {
    *bc++ = XY_FAST_COPY;
    *bc++ = FAST_COPY_32BPP | dst_pitch;              /* linear */
    *bc++ = ((uint32_t)y0 << 16) | (uint32_t)x0;
    *bc++ = ((uint32_t)y1 << 16) | (uint32_t)x1;
    *bc++ = dst;
    *bc++ = 0;
    *bc++ = ((uint32_t)sy << 16) | (uint32_t)sx;
    *bc++ = src_pitch;
    *bc++ = src;
    *bc++ = 0;
}

static void batch_flush(void) {
    *bc++ = MI_FLUSH_DW;                              /* what came before reaches memory */
    *bc++ = 0;
    *bc++ = 0;
    *bc++ = 0;
}

static uint32_t batch_submit(void) {
    *bc++ = MI_STORE_GGTT;                            /* then: this batch's number */
    *bc++ = ctx_data_g;
    *bc++ = 0;
    *bc++ = ++seqno;
    if ((bc - bstart) & 1) *bc++ = MI_NOOP;
    ring_tail += (uint32_t)(bc - bstart) * 4;
    if (ring_tail >= ring_bytes) ring_tail = 0;
    ctx_regs[7] = ring_tail;                          /* the context's RING_TAIL */
    __asm__ volatile("sfence" ::: "memory");
    fw_get();                                         /* awake until igfx_copy_idle */
    mm_wr(BCS + EXECLIST_SQ_LO, ctx_desc_lo);
    mm_wr(BCS + EXECLIST_SQ_HI, ctx_desc_hi);
    mm_wr(BCS + EXECLIST_CONTROL, 1);
    return seqno;
}

int igfx_copy_submit(const uint32_t *src, int buffer, const int *rects, int n,
                     const struct igfx_pre *pre, int npre, uint32_t *seq) {
    if (!blitter_ok || src != mapped_shadow || buffer < 1 || buffer > 2 || n + npre <= 0) return -1;
    if (batch_begin((uint32_t)(n + npre) * 10)) return -1;
    if (npre) {                                       /* window contents into the frame first */
        for (int i = 0; i < npre; i++)
            batch_copy(VA_SHADOW + src_off, src_pitch, pre[i].x, pre[i].y, pre[i].x + pre[i].w, pre[i].y + pre[i].h,
                       pre[i].src_va, pre[i].src_pitch, pre[i].sx, pre[i].sy);
        batch_flush();
    }
    uint32_t dst = buffer == 1 ? VA_BUF1 : VA_BUF2;
    for (int i = 0; i < n; i++)
        batch_copy(dst, pitch, rects[4 * i], rects[4 * i + 1], rects[4 * i + 2], rects[4 * i + 3],
                   VA_SHADOW + src_off, src_pitch, rects[4 * i], rects[4 * i + 1]);
    batch_flush();
    *seq = batch_submit();
    return 0;
}

int igfx_copy_done(uint32_t seq) { return (int32_t)(*(volatile uint32_t *)ctx_data - seq) >= 0; }

void igfx_copy_idle(void) { fw_put(); }

void igfx_copy_failed(void) {
    fw_get();
    blitter_off("a copy did not finish");
    fw_put();
}

/* ---- The render engine (GPU driver stages 2-3: shaders) ---------------------
 * The desktop's effects run as shaders (gpu/shaders.cl, compiled by Intel's
 * compiler into drivers/gpu_shaders.h) on the render engine in its GPGPU
 * pipeline: PIPELINE_SELECT, STATE_BASE_ADDRESS (the code at VA_CODE, the
 * interface descriptors at VA_STATE), MEDIA_VFE_STATE, then per pass a
 * MEDIA_INTERFACE_DESCRIPTOR_LOAD and a GPGPU_WALKER (SIMD16, one thread a
 * group: 16 pixels of a row), a flush between passes and the batch's number
 * written last (PIPE_CONTROL). The shaders read nothing from their payload but
 * r0 (the thread group's x, y, z): their parameters are 64-byte blocks at
 * VA_PARAMS, the group's z picking one. The context is restore-inhibited, so
 * every batch sets the whole state again. Memory is the blitter's page tables
 * (PPGTT), shared: programs' buffers are mapped in four slots of their own.
 * At boot every shader is checked against the CPU's arithmetic on test
 * pictures; the desktop gets the GPU only if all agree (gfx GFX_GPU). */
#include "drivers/gpu_shaders.h"
#include "sys/syscall_nums.h"

#define VA_CODE          0x3C000000u            /* the shaders' machine code (instruction base) */
#define VA_PARAMS        0x3C100000u            /* = PARAMS in shaders.cl */
#define VA_STATE         0x3C200000u            /* interface descriptors (dynamic state base) */
#define VA_TEST          0x3C300000u            /* the self-tests' pictures */
#define VA_USER(i)       (0x34000000u + 0x01000000u * (uint32_t)(i))   /* programs' buffers, 16 MiB each */
#define CODE_PAGES       24
#define TEST_PAGES       16
#define MAX_JOBS         64

#define PIPE_CONTROL     0x7A000004u
#define PC0_HDC_FLUSH    (1u << 9)              /* (in the first dword, Gen12) */
#define PC_CS_STALL      (1u << 20)
#define PC_TLB_INVAL     (1u << 18)
#define PC_WRITE_IMM     (1u << 14)
#define PC_ICACHE_INVAL  (1u << 11)
#define PC_DC_FLUSH      (1u << 5)
#define PC_STATE_INVAL   (1u << 2)
#define PC_GGTT          (1u << 24)
#define PIPELINE_GPGPU   0x69040302u
#define SBA              0x61010014u            /* STATE_BASE_ADDRESS, 22 dwords (Gen11+) */
#define VFE_STATE        0x70000007u
#define IDD_LOAD         0x70020002u
#define GPGPU_WALKER     0x7105000Du
#define MEDIA_FLUSH      0x70040000u

typedef struct {                                /* = params_t in shaders.cl */
    uint64_t dst, src;
    uint32_t dst_pitch, src_pitch, w, h, a, b, c, d, e, f;
} gpu_params_t;

static int gpu_ok;
static uint32_t gpu_good;                       /* the shaders that passed their test (1 << GPU_*) */
#define GOOD(k) (gpu_ok && (gpu_good >> (k) & 1))
static char gpu_status_text[120] = "not tried";
static uint32_t *code_cpu, *state_cpu, *test_cpu;
static gpu_params_t *params_cpu;
static uint32_t *gc, *gstart;                   /* the batch being written into the render ring */
static int gjobs;
static uint32_t gseq;

const char *igfx_gpu_status(void) { return gpu_status_text; }
int igfx_gpu_ready(void) { return gpu_ok; }

/* `pages` pages of kernel memory, page aligned, mapped at `va` in the PPGTT. */
static void *gpu_pages(uint32_t va, int pages) {
    uint8_t *raw = kmalloc((uint64_t)(pages + 1) * 4096);
    if (!raw) return 0;
    uint8_t *p = (uint8_t *)(((uint64_t)raw + 4095) & ~4095ULL);
    memset(p, 0, (uint64_t)pages * 4096);
    return ppgtt_map_range(va, p, (uint64_t)pages * 4096) == 0 ? p : 0;
}

static int flush_opt = -1;                      /* clflushopt (not ordered among themselves: faster) */

static void flush_lines(uint64_t a, uint64_t end) {
    if (flush_opt < 0) {
        uint32_t ax = 0, bx, cx = 0, dx;
        __asm__ volatile("cpuid" : "+a"(ax), "=b"(bx), "+c"(cx), "=d"(dx));
        flush_opt = 0;
        if (ax >= 7) {
            ax = 7;
            cx = 0;
            __asm__ volatile("cpuid" : "+a"(ax), "=b"(bx), "+c"(cx), "=d"(dx));
            flush_opt = (bx >> 23) & 1;
        }
    }
    a &= ~63ULL;
    if (flush_opt)
        for (; a < end; a += 64) __asm__ volatile("clflushopt (%0)" :: "r"(a) : "memory");
    else
        for (; a < end; a += 64) __asm__ volatile("clflush (%0)" :: "r"(a) : "memory");
}

static void clflush_range(const void *p, uint64_t bytes) {
    flush_lines((uint64_t)p, (uint64_t)p + bytes);
    __asm__ volatile("mfence" ::: "memory");
}

/* w x h pixels from p, rows `stride` pixels apart: only the rows' pieces. */
static void clflush_rect(const uint32_t *p, int stride, int w, int h) {
    for (int y = 0; y < h; y++) {
        uint64_t a = (uint64_t)(p + (uint64_t)y * stride);
        flush_lines(a, a + (uint64_t)w * 4);
    }
    __asm__ volatile("mfence" ::: "memory");
}

static void pc(uint32_t dw0_extra, uint32_t flags, uint32_t addr, uint32_t value) {
    *gc++ = PIPE_CONTROL | dw0_extra;
    *gc++ = flags;
    *gc++ = addr;
    *gc++ = 0;
    *gc++ = value;
    *gc++ = 0;
}

/* A batch: everything the restore-inhibited context forgot, then passes. */
static void gpu_begin(void) {
    uint32_t need = 1024 * 4, ring_size = RING_PAGES * 4096;
    if (rcs_tail + need > ring_size) {
        while (rcs_tail < ring_size) rcs_ring[rcs_tail / 4] = MI_NOOP, rcs_tail += 4;
        rcs_tail = 0;
    }
    gc = gstart = rcs_ring + rcs_tail / 4;
    gjobs = 0;
    /* Forget old translations (programs' buffers move between batches). */
    pc(0, PC_CS_STALL | PC_TLB_INVAL | PC_WRITE_IMM | PC_GGTT, rcs_data_g + 8, 0);
    pc(PC0_HDC_FLUSH, PC_CS_STALL | PC_DC_FLUSH, 0, 0);
    *gc++ = PIPELINE_GPGPU;
    *gc++ = SBA;
    *gc++ = 0 | 1;          *gc++ = 0;          /* general state: 0 */
    *gc++ = 0;                                  /* stateless MOCS: 0 */
    *gc++ = 0 | 1;          *gc++ = 0;          /* surface state: 0 */
    *gc++ = VA_STATE | 1;   *gc++ = 0;          /* dynamic state: the interface descriptors */
    *gc++ = 0 | 1;          *gc++ = 0;          /* indirect objects: 0 */
    *gc++ = VA_CODE | 1;    *gc++ = 0;          /* instructions: the shaders */
    *gc++ = 0xFFFFF000u | 1;                    /* the four sizes: all of it */
    *gc++ = 0xFFFFF000u | 1;
    *gc++ = 0xFFFFF000u | 1;
    *gc++ = 0xFFFFF000u | 1;
    *gc++ = 0 | 1;          *gc++ = 0;          /* bindless surfaces: none */
    *gc++ = 0;
    *gc++ = 0 | 1;          *gc++ = 0;          /* bindless samplers: none */
    *gc++ = 0;
    pc(0, PC_CS_STALL | PC_STATE_INVAL | PC_ICACHE_INVAL, 0, 0);
    *gc++ = VFE_STATE;
    *gc++ = 0;                                  /* no scratch space */
    *gc++ = 0;
    *gc++ = (335u << 16) | (2u << 8) | (1u << 7);   /* threads (48 EUs x 7 - 1), 2 URB entries, gateway timer reset */
    *gc++ = 0;
    *gc++ = (2u << 16) | 0;                     /* URB entry size 2, no CURBE */
    *gc++ = 0;
    *gc++ = 0;
    *gc++ = 0;
}

/* One pass: kernel `k` over gx x gy thread groups with parameters `p`. */
static void gpu_pass(int k, const gpu_params_t *p, uint32_t gx, uint32_t gy) {
    int job = gjobs++;
    params_cpu[job] = *p;
    *gc++ = IDD_LOAD;
    *gc++ = 0;
    *gc++ = 32;                                 /* one descriptor */
    *gc++ = (uint32_t)k * 64;                   /* its offset from VA_STATE */
    *gc++ = GPGPU_WALKER;
    *gc++ = 0;                                  /* descriptor 0 of those loaded */
    *gc++ = 0;                                  /* no indirect data */
    *gc++ = 0;
    *gc++ = 1u << 30;                           /* SIMD16, one thread a group */
    *gc++ = 0;  *gc++ = 0;  *gc++ = gx;         /* x: from 0, gx groups */
    *gc++ = 0;  *gc++ = 0;  *gc++ = gy;         /* y */
    *gc++ = (uint32_t)job; *gc++ = 1;           /* z: this job's parameter block */
    *gc++ = 0xFFFF;                             /* right execution mask (16 lanes; shaders check x < w) */
    *gc++ = 0xFFFFFFFFu;
    *gc++ = MEDIA_FLUSH;
    *gc++ = 0;
    pc(PC0_HDC_FLUSH, PC_CS_STALL | PC_DC_FLUSH, 0, 0);   /* the next pass reads what this one wrote */
}

/* Where a batch's time goes (into the log every 400 batches): 0 preparing
 * (mapping, flushing the CPU's caches), 1 waking the GPU, 2 running, 3
 * flushing after. In TSC ticks; gtsc_per_ms from igfx_gpu_init. */
static uint64_t gtsc_per_ms, gst_sum[4], gst_max[4];
static uint32_t gst_n;

static uint64_t gtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

static void gst_add(int phase, uint64_t t) {
    gst_sum[phase] += t;
    if (t > gst_max[phase]) gst_max[phase] = t;
}

static void gst_batch(void) {
    if (++gst_n < 400 || !gtsc_per_ms) return;
    uint64_t us = gtsc_per_ms / 1000 ? gtsc_per_ms / 1000 : 1;
    char line[200];
    k_snprintf(line, sizeof(line), "igfx: gpu: %u batches, us avg/max: prepare %u/%u, wake %u/%u, run %u/%u, after %u/%u\n",
               gst_n, (uint32_t)(gst_sum[0] / gst_n / us), (uint32_t)(gst_max[0] / us),
               (uint32_t)(gst_sum[1] / gst_n / us), (uint32_t)(gst_max[1] / us),
               (uint32_t)(gst_sum[2] / gst_n / us), (uint32_t)(gst_max[2] / us),
               (uint32_t)(gst_sum[3] / gst_n / us), (uint32_t)(gst_max[3] / us));
    print_log(line);
    gst_n = 0;
    for (int i = 0; i < 4; i++) gst_sum[i] = gst_max[i] = 0;
}

/* Submit, wait (at most 50 ms). -> 0 done; on a timeout the GPU is off. */
static int gpu_run(const char *what) {
    pc(PC0_HDC_FLUSH, PC_CS_STALL | PC_DC_FLUSH | PC_WRITE_IMM | PC_GGTT, rcs_data_g, ++gseq);
    if ((gc - gstart) & 1) *gc++ = MI_NOOP;
    rcs_tail += (uint32_t)(gc - gstart) * 4;
    if (rcs_tail >= RING_PAGES * 4096) rcs_tail = 0;
    rcs_regs[7] = rcs_tail;
    clflush_range(params_cpu, MAX_JOBS * 64);
    clflush_range(gstart, (uint64_t)(gc - gstart) * 4);
    clflush_range(&rcs_regs[7], 4);
    uint64_t tw = gtsc();
    mm_wr(FORCEWAKE_RENDER, 0x00010001);
    uint64_t end = timer_ms() + 2;
    while (!(mm(FORCEWAKE_RENDER_ACK) & 1) && timer_ms() <= end) __asm__ volatile("pause");
    gst_add(1, gtsc() - tw);
    mm_wr(RCS + EXECLIST_SQ_LO, rcs_desc_lo);
    mm_wr(RCS + EXECLIST_SQ_HI, rcs_desc_hi);
    mm_wr(RCS + EXECLIST_CONTROL, 1);
    uint64_t tr = gtsc();
    end = timer_ms() + 50;
    while ((int32_t)(*(volatile uint32_t *)rcs_data - gseq) < 0 && timer_ms() < end) __asm__ volatile("pause");
    gst_add(2, gtsc() - tr);
    int done = (int32_t)(*(volatile uint32_t *)rcs_data - gseq) >= 0;
    if (!done) {
        char line[220];
        k_snprintf(line, sizeof(line), "igfx: gpu: %s did not finish: head %x tail %x acthd %x ipehr %x execlist %x:%x\n",
                   what, mm(RCS + RING_HEAD), mm(RCS + RING_TAIL), mm(RCS + 0x74), mm(RCS + 0x68),
                   mm(RCS + EXECLIST_STATUS), mm(RCS + EXECLIST_STATUS + 4));
        print_log(line);
        gpu_ok = 0;
        k_snprintf(gpu_status_text, sizeof(gpu_status_text), "off: %s did not finish", what);
    }
    mm_wr(FORCEWAKE_RENDER, 0x00010000);
    return done ? 0 : -1;
}

/* ---- The CPU's arithmetic, for the self-tests -------------------------------- */
static uint32_t ref_lerp(uint32_t a, uint32_t b, uint32_t f) {
    if (f > 256) f = 256;
    uint32_t r = (((a >> 16) & 255) * (256 - f) + ((b >> 16) & 255) * f) >> 8;
    uint32_t g = (((a >> 8) & 255) * (256 - f) + ((b >> 8) & 255) * f) >> 8;
    uint32_t bl = ((a & 255) * (256 - f) + (b & 255) * f) >> 8;
    return r << 16 | g << 8 | bl;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint32_t ref_box3(const uint32_t *s, int pitch, int w, int h, int x, int y, int horizontal) {
    uint32_t r = 0, g = 0, b = 0;
    for (int d = -1; d <= 1; d++) {
        int xx = horizontal ? clampi(x + d, 0, w - 1) : x, yy = horizontal ? y : clampi(y + d, 0, h - 1);
        uint32_t c = s[yy * pitch + xx];
        r += c >> 16 & 255;
        g += c >> 8 & 255;
        b += c & 255;
    }
    return (r / 3) << 16 | (g / 3) << 8 | (b / 3);
}

static uint32_t ref_satint(uint32_t c, int sat, uint32_t tint, int alpha) {
    int r = (int)(c >> 16 & 255), g = (int)(c >> 8 & 255), b = (int)(c & 255);
    int gray = (r * 30 + g * 59 + b * 11) / 100;
    r = clampi(gray + (r - gray) * sat / 100, 0, 255);
    g = clampi(gray + (g - gray) * sat / 100, 0, 255);
    b = clampi(gray + (b - gray) * sat / 100, 0, 255);
    r = (r * (255 - alpha) + (int)(tint >> 16 & 255) * alpha) / 255;
    g = (g * (255 - alpha) + (int)(tint >> 8 & 255) * alpha) / 255;
    b = (b * (255 - alpha) + (int)(tint & 255) * alpha) / 255;
    return (uint32_t)(r << 16 | g << 8 | b);
}

static uint32_t ref_upscale(const uint32_t *s, int pitch, int bw, int bh, int bleed, int i, int j) {
    int u = (i + bleed) * 64 - 96, c0 = u >> 8;
    uint32_t f = (uint32_t)(u & 255);
    if (c0 < 0) { c0 = 0; f = 0; }
    if (c0 >= bw - 1) { c0 = bw - 1; f = 0; }
    int v = (j + bleed) * 64 - 96, r0 = v >> 8;
    uint32_t rf = (uint32_t)(v & 255);
    if (r0 < 0) { r0 = 0; rf = 0; }
    if (r0 >= bh - 1) { r0 = bh - 1; rf = 0; }
    const uint32_t *a = s + r0 * pitch, *b = rf ? a + pitch : a;
    uint32_t top = f ? ref_lerp(a[c0], a[c0 + 1], f) : a[c0];
    uint32_t bot = f ? ref_lerp(b[c0], b[c0 + 1], f) : b[c0];
    return rf ? ref_lerp(top, bot, rf) : top;
}

/* ui.c's arithmetic (corners, shadows) and the window server's (layers, scaling). */
static uint32_t div255(uint32_t v) { return (v + 1 + (v >> 8)) >> 8; }

static uint32_t ref_mix(uint32_t dst, uint32_t color, uint32_t a) {
    if (a >= 255) return color;
    uint32_t na = 255 - a;
    uint32_t r = div255(((color >> 16) & 255) * a + ((dst >> 16) & 255) * na);
    uint32_t g = div255(((color >> 8) & 255) * a + ((dst >> 8) & 255) * na);
    uint32_t b = div255((color & 255) * a + (dst & 255) * na);
    return r << 16 | g << 8 | b;
}

static int ref_isqrt(int v) {
    if (v <= 0) return 0;
    int r = 0, bit = 1 << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

static uint32_t ref_rect_cover(int i, int j, int w, int h, int r, int corners) {
    int ci = -1, cj = -1;
    if (i < r) ci = i;
    else if (i >= w - r) ci = w - 1 - i;
    if (j < r) cj = j;
    else if (j >= h - r) cj = h - 1 - j;
    if (ci < 0 || cj < 0) return 255;
    int which = (j < r ? (i < r ? 1 : 2) : (i < r ? 4 : 8));
    if (!(corners & which)) return 255;
    int R = 8 * r, inside = 0;
    for (int sy = 0; sy < 4; sy++) {
        int dy = R - (8 * cj + 2 * sy + 1);
        for (int sx = 0; sx < 4; sx++) {
            int dx = R - (8 * ci + 2 * sx + 1);
            if (dx <= 0 && dy <= 0) inside++;
            else if ((dx > 0 ? dx * dx : 0) + (dy > 0 ? dy * dy : 0) <= R * R) inside++;
        }
    }
    return (uint32_t)(inside * 255 / 16);
}

static uint32_t ref_shadow(uint32_t dst, int px, int py, int x, int y, int w, int h, int r, int size, int alpha, int outside) {
    int hs = size / 2;
    if (py >= y + hs && py < y + h - hs && px >= x + r + hs && px < x + w - r - hs) return dst;
    int qy = y + r - py;
    if (py - (y + h - 1 - r) > qy) qy = py - (y + h - 1 - r);
    if (qy < 0) qy = 0;
    int qx = x + r - px;
    if (px - (x + w - 1 - r) > qx) qx = px - (x + w - 1 - r);
    if (qx < 0) qx = 0;
    int d = ref_isqrt((qx * qx + qy * qy) * 16) - r * 4;
    if (d <= 0 && outside) return dst;
    if (d <= 0) {
        int ix = px - x < x + w - 1 - px ? px - x : x + w - 1 - px;
        int iy = py - y < y + h - 1 - py ? py - y : y + h - 1 - py;
        int in = ix < iy ? ix : iy;
        if (in > r && qx == 0 && qy == 0) d = -in * 4;
    }
    int span = size * 6, f = size * 4 - d;
    if (f <= 0) return dst;
    if (f > span) f = span;
    uint32_t a = (uint32_t)(alpha * f / span * f / span);
    return a ? ref_mix(dst, 0, a) : dst;
}

static uint32_t ref_over(uint32_t d, uint32_t v) {
    uint32_t a = v >> 24, k = 255 - a;
    if (!a) return d;
    if (a == 255) return v & 0xFFFFFF;
    uint32_t r = ((v >> 16) & 255) + ((d >> 16) & 255) * k / 255;
    uint32_t g = ((v >> 8) & 255) + ((d >> 8) & 255) * k / 255;
    uint32_t bl = (v & 255) + (d & 255) * k / 255;
    return (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (bl > 255 ? 255 : bl);
}

/* blit_scaled at pixel (i, j) of the dw x dh rectangle; dst: what is there,
 * under: what was there before the shadow (or dst). */
static uint32_t ref_scaled(const uint32_t *col, const uint32_t *mask, int ow, int oh, int dw, int dh,
                           int i, int j, uint32_t alpha, uint32_t dst, uint32_t under) {
    int v = ((2 * j + 1) * oh * 128) / dh - 128, ty = v >> 8;
    uint32_t fy = (uint32_t)(v & 255);
    if (ty < 0) { ty = 0; fy = 0; }
    if (ty >= oh - 1) { ty = oh - 1; fy = 0; }
    int u = ((2 * i + 1) * ow * 128) / dw - 128, tx = u >> 8;
    uint32_t fx = (uint32_t)(u & 255);
    if (tx < 0) { tx = 0; fx = 0; }
    if (tx >= ow - 1) { tx = ow - 1; fx = 0; }
    const uint32_t *c0 = col + ty * ow, *c1 = fy ? c0 + ow : c0, *m0 = mask + ty * ow, *m1 = fy ? m0 + ow : m0;
    uint32_t c, m;
    if (!fx && !fy) {
        c = c0[tx];
        m = m0[tx] & 255;
    } else {
        uint32_t a = fx ? ref_lerp(c0[tx], c0[tx + 1], fx) : c0[tx], b = fx ? ref_lerp(c1[tx], c1[tx + 1], fx) : c1[tx];
        c = fy ? ref_lerp(a, b, fy) : a;
        a = fx ? ref_lerp(m0[tx], m0[tx + 1], fx) : m0[tx];
        b = fx ? ref_lerp(m1[tx], m1[tx + 1], fx) : m1[tx];
        m = (fy ? ref_lerp(a, b, fy) : a) & 255;
    }
    uint32_t a = m * alpha / 255;
    if (a >= 255) return c;
    if (!a) return dst;
    return ref_lerp(m >= 255 ? under : dst, c, a + (a >> 7));
}

/* ---- Bring-up and self-tests ----------------------------------------------------- */
#define T_SRC   0                               /* test pictures in VA_TEST: 64 pixels a row, 16 rows */
#define T_DST   (64 * 16)
#define T_PITCH 64
#define T_MASK  (2 * 64 * 16)                   /* (scaled: the shape, and what was under it) */
#define T_UNDER (3 * 64 * 16)

static uint32_t t_src[64 * 16], t_dst[64 * 16]; /* CPU copies: the source, the destination before */
static uint32_t t_mask[64 * 16];

static int check(const char *name, int w, int h, uint32_t (*want)(int x, int y)) {
    __asm__ volatile("wbinvd" ::: "memory");      /* (the GPU wrote around the CPU's caches) */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t got = test_cpu[T_DST + y * T_PITCH + x], w0 = want(x, y);
            if (got != w0) {
                char line[160];
                k_snprintf(line, sizeof(line), "igfx: gpu test %s: WRONG at %d,%d: %x, the CPU says %x\n", name, x, y, got, w0);
                print_log(line);
                return -1;
            }
        }
    char line[80];
    k_snprintf(line, sizeof(line), "igfx: gpu test %s: ok\n", name);
    print_log(line);
    return 0;
}

static uint32_t want_fill(int x, int y) { (void)x; (void)y; return 0x00C0FFEEu; }
static uint32_t want_box_h(int x, int y) { return ref_box3(t_src, T_PITCH, 37, 7, x, y, 1); }
static uint32_t want_box_v(int x, int y) { return ref_box3(t_src, T_PITCH, 37, 7, x, y, 0); }
static uint32_t want_satint(int x, int y) { return ref_satint(t_src[y * T_PITCH + x], 180, 0xF2F4F8, 168); }
static uint32_t want_upscale(int x, int y) { return ref_upscale(t_src, T_PITCH, 12, 6, 20, x, y + 3); }
static uint32_t picture(int x, int y) {
    return ((uint32_t)(x * 37 + y * 11) & 255) << 16 | ((uint32_t)(x * 5 + y * 71) & 255) << 8 | ((uint32_t)(x * y * 3) & 255);
}
static uint32_t fade_old(int x, int y) { return (uint32_t)(x * 0x030507 + y * 0x110000) & 0xFFFFFF; }
static uint32_t want_fade(int x, int y) { return ref_lerp(fade_old(x, y), picture(x, y), 100); }
static uint32_t blank(int x, int y) { (void)x; (void)y; return 0xDEADBEEF; }
static uint32_t layer_pic(int x, int y) {               /* premultiplied-ish, with 0 and 255 alphas */
    uint32_t a = (uint32_t)(x * 29 + y * 53) & 255;
    if (x % 5 == 0) a = 255;
    if (x % 7 == 0) a = 0;
    return a << 24 | picture(x, y);
}
static uint32_t mask_pic(int x, int y) {
    uint32_t m = (x < 6 || y < 2 || x > 50) ? (uint32_t)(x * 45 + y * 20) & 255 : 255;
    return m * 0x010101u;
}
static uint32_t want_shadow(int x, int y) { return ref_shadow(t_dst[y * T_PITCH + x], x, y, 6, 3, 26, 8, 3, 5, 200, 0); }
static uint32_t want_shadow_out(int x, int y) { return ref_shadow(t_dst[y * T_PITCH + x], x, y, -4, -2, 30, 12, 6, 8, 150, 1); }
static uint32_t want_over(int x, int y) {
    uint32_t d = t_dst[y * T_PITCH + x];
    return (x >= 5 && x < 20 && y >= 2 && y < 5) ? d : ref_over(d, t_src[y * T_PITCH + x]);
}
static uint32_t want_rcopy(int x, int y) {
    int i = x + 1, j = y + 2, w = 30, h = 10, r = 4;
    uint32_t from = t_src[y * T_PITCH + x];
    if (j >= r && j < h - r) return from;
    uint32_t a = (i < r || i >= w - r) ? ref_rect_cover(i, j, w, h, r, 9) : 255;
    return ref_mix(t_dst[y * T_PITCH + x], from, a);
}
static uint32_t want_rfill(int x, int y) {
    int i = x, j = y + 1, w = 31, h = 9, r = 4;
    uint32_t cover = ((j < r || j >= h - r) && (i < r || i >= w - r)) ? ref_rect_cover(i, j, w, h, r, 6) : 255;
    uint32_t d = t_dst[y * T_PITCH + x], a = cover * 170 / 255;
    return a ? ref_mix(d, 0x3A6F9C, a) : d;
}
static uint32_t want_scaled(int x, int y) {
    return ref_scaled(t_src, t_mask, 64, 12, 50, 14, x + 3, y + 1, 180, t_dst[y * T_PITCH + x], fade_old(x, y));
}

static gpu_params_t tparams(uint32_t w, uint32_t h) {
    gpu_params_t p;
    memset(&p, 0, sizeof(p));
    p.dst = VA_TEST + T_DST * 4;
    p.src = VA_TEST + T_SRC * 4;
    p.dst_pitch = p.src_pitch = T_PITCH;
    p.w = w;
    p.h = h;
    return p;
}

/* One test: kernel k (gx x gy groups) on the test picture, then compare.
 * fade: the destination starts as the picture, the source is the old one. */
static int run_test2(const char *name, int k, gpu_params_t p, int w, int h, uint32_t (*want)(int, int),
                     uint32_t (*src)(int, int), uint32_t (*dst)(int, int)) {
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 64; x++) {
            int i = y * T_PITCH + x;
            test_cpu[T_SRC + i] = t_src[i] = src(x, y);
            test_cpu[T_DST + i] = t_dst[i] = dst(x, y);
            test_cpu[T_MASK + i] = t_mask[i] = mask_pic(x, y);
            test_cpu[T_UNDER + i] = 0;
        }
    for (int y = 0; y < 16; y++)                     /* (packed, the region's width) */
        for (int x = 0; x < (int)p.w; x++) test_cpu[T_UNDER + y * (int)p.w + x] = fade_old(x, y);
    __asm__ volatile("wbinvd" ::: "memory");
    gpu_begin();
    gpu_pass(k, &p, ((uint32_t)w + 15) / 16, (uint32_t)h);
    if (gpu_run(name)) return -1;
    if (check(name, w, h, want)) return -1;
    gpu_good |= 1u << k;
    return 0;
}

static int run_test(const char *name, int k, gpu_params_t p, int w, int h, uint32_t (*want)(int, int)) {
    return k == GPU_FADE ? run_test2(name, k, p, w, h, want, fade_old, picture)
                         : run_test2(name, k, p, w, h, want, picture, blank);
}

int igfx_gpu_init(void) {
    if (!render_ok) {
        k_snprintf(gpu_status_text, sizeof(gpu_status_text), "off: the render engine did not answer");
        return -1;
    }
    if (!ppgtt_pd && blitter_ok) blitter_tables(0);  /* the page tables: the blitter's, or our own */
    if (!ppgtt_pd) {
        uint64_t pd = page_alloc();
        if (!pd) return -1;
        ppgtt_pd = phys_to_virt(pd);
        memset(ppgtt_pd, 0, 4096);
    }
    uint64_t m0 = timer_ms(), spins = 0;             /* the TSC's rate, for the batch times (20 ms) */
    while (timer_ms() == m0 && ++spins < 400000000ULL) __asm__ volatile("pause");
    uint64_t ts = gtsc(), m1 = timer_ms();
    while (timer_ms() < m1 + 20 && ++spins < 800000000ULL) __asm__ volatile("pause");
    if (timer_ms() >= m1 + 20) gtsc_per_ms = (gtsc() - ts) / (timer_ms() - m1);
    uint64_t pd = phys_of((uint64_t)ppgtt_pd);
    rcs_regs[49] = (uint32_t)(pd >> 32);             /* the render context's PDP0 */
    rcs_regs[51] = (uint32_t)pd;
    code_cpu = gpu_pages(VA_CODE, CODE_PAGES);
    params_cpu = gpu_pages(VA_PARAMS, 1);
    state_cpu = gpu_pages(VA_STATE, 1);
    test_cpu = gpu_pages(VA_TEST, TEST_PAGES);
    if (!code_cpu || !params_cpu || !state_cpu || !test_cpu) {
        k_snprintf(gpu_status_text, sizeof(gpu_status_text), "off: out of memory");
        print_log("igfx: gpu: off: out of memory\n");
        return -1;
    }
    uint32_t off = 0;
    for (int k = 0; k < GPU_KERNELS; k++) {          /* the code, 64-byte aligned; a descriptor each */
        if (off + gpu_kernels[k].size > CODE_PAGES * 4096) return -1;
        memcpy((uint8_t *)code_cpu + off, gpu_kernels[k].isa, gpu_kernels[k].size);
        uint32_t *idd = state_cpu + k * 16;
        idd[0] = off;                                /* kernel start (from VA_CODE) */
        idd[1] = 0;
        idd[2] = 0;                                  /* IEEE floats, multiple program flow */
        idd[3] = 0;                                  /* no samplers */
        idd[4] = 0;                                  /* no binding table */
        idd[5] = 0;                                  /* no per-thread data */
        idd[6] = 1;                                  /* one thread a group, no shared memory */
        idd[7] = 0;                                  /* no cross-thread data */
        off = (off + gpu_kernels[k].size + 63) & ~63u;
    }
    __asm__ volatile("wbinvd" ::: "memory");          /* tables, code, descriptors to memory */

    gpu_ok = 1;                                      /* (gpu_run turns it off on a hang) */
    int bad = 0;
    gpu_params_t p = tparams(40, 5);
    p.a = 0x00C0FFEE;
    bad |= run_test("fill", GPU_FILL, p, 40, 5, want_fill);
    if (gpu_ok && !bad) {
        p = tparams(37, 7);
        p.a = 1;
        bad |= run_test("box3 across", GPU_BOX3, p, 37, 7, want_box_h);
        p.a = 0;
        bad |= run_test("box3 down", GPU_BOX3, p, 37, 7, want_box_v);
        p = tparams(37, 7);
        p.a = 180; p.b = 0xF2F4F8; p.c = 168;
        bad |= run_test("satint", GPU_SATINT, p, 37, 7, want_satint);
        p = tparams(40, 10);
        p.a = 12; p.b = 6; p.c = 20; p.d = 3;
        bad |= run_test("upscale", GPU_UPSCALE, p, 40, 10, want_upscale);
        p = tparams(37, 7);
        p.a = 100;
        bad |= run_test("fade", GPU_FADE, p, 37, 7, want_fade);
        if (bad) gpu_good &= ~(1u << GPU_BOX3);      /* (two tests share it) */
        int bad2 = 0;
        p = tparams(40, 14);
        p.a = 6; p.b = 3; p.c = 26; p.d = 8; p.e = 3 | 5u << 16; p.f = 200;
        bad2 |= run_test2("shadow", GPU_SHADOW, p, 40, 14, want_shadow, picture, fade_old);
        p.a = (uint32_t)-4; p.b = (uint32_t)-2; p.c = 30; p.d = 12; p.e = 6 | 8u << 16; p.f = 150 | 1u << 16;
        bad2 |= run_test2("shadow outside", GPU_SHADOW, p, 40, 14, want_shadow_out, picture, fade_old);
        if (bad2) gpu_good &= ~(1u << GPU_SHADOW);
        p = tparams(37, 7);
        p.a = 5; p.b = 2; p.c = 20; p.d = 5;
        run_test2("over", GPU_OVER, p, 37, 7, want_over, layer_pic, fade_old);
        p = tparams(37, 7);
        p.a = (uint32_t)-1; p.b = (uint32_t)-2; p.c = 30; p.d = 10; p.e = 4; p.f = 9;
        run_test2("round", GPU_RCOPY, p, 37, 7, want_rcopy, picture, fade_old);
        p = tparams(40, 10);
        p.a = (uint32_t)-3; p.b = (uint32_t)-1; p.c = 50 | 14u << 16; p.d = 12 | 180u << 16;
        p.e = VA_TEST + T_MASK * 4; p.f = VA_TEST + T_UNDER * 4;
        run_test2("scaled", GPU_SCALED, p, 40, 10, want_scaled, picture, fade_old);
        p = tparams(31, 8);                          /* (the rectangle 0, -1, 31 x 9) */
        p.a = 0; p.b = (uint32_t)-1; p.c = 31; p.d = 9; p.e = 4; p.f = 6 | 170u << 8; p.src = 0x3A6F9C;
        run_test2("rounded fill", GPU_RFILL, p, 31, 8, want_rfill, picture, fade_old);
    }
    int n = 0;
    for (int k = 0; k < GPU_KERNELS; k++) n += gpu_good >> k & 1;
    if (gpu_ok && n) {
        k_snprintf(gpu_status_text, sizeof(gpu_status_text), "shaders on the render engine (%d of %d checked)", n, GPU_KERNELS);
    } else {
        if (gpu_ok) k_snprintf(gpu_status_text, sizeof(gpu_status_text), "off: a shader gave a wrong picture");
        gpu_ok = 0;
    }
    char line[160];
    k_snprintf(line, sizeof(line), "igfx: gpu: %s\n", gpu_status_text);
    print_log(line);
    return gpu_ok ? 0 : -1;
}

/* ---- Programs' work (gfx GFX_GPU) --------------------------------------------- */

/* A program's request the GPU will not do (the program draws it): the first
 * few into the log, with the line of the check that said no. */
static int refused(int line) {
    static int n;
    if (n++ < 24) {
        char l[80];
        k_snprintf(l, sizeof(l), "igfx: gpu: refused (igfx.c line %d)\n", line);
        print_log(l);
    }
    return -1;
}

/* A program's buffer [va, va + bytes) at GPU address `at` (`max` bytes of room) -> its GPU address, 0. */
static uint32_t map_at(uint32_t at, uint64_t max, uint64_t root, uint64_t va, uint64_t bytes) {
    uint64_t first = va & ~0xFFFULL, span = (va & 0xFFF) + bytes;
    if (!bytes || span > max) return 0;
    for (uint64_t off = 0; off < span; off += 4096) {
        uint64_t e = paging_get_user_entry(root, first + off);
        if (!(e & 1)) return 0;                      /* (not touched yet: the CPU does this one) */
        pte_set(at + (uint32_t)off, e & 0x000FFFFFFFFFF000ULL);
    }
    __asm__ volatile("mfence" ::: "memory");
    return at + (uint32_t)(va & 0xFFF);
}

/* ... into slot i (16 MiB). */
static uint32_t map_user(int i, uint64_t root, uint64_t va, uint64_t bytes) {
    return map_at(VA_USER(i), 0x01000000u, root, va, bytes);
}

/* Frosted glass: samp (bw x bh) blurred (four box passes, scratch the same
 * size), saturated and tinted, then rows j0..j1 of the w-wide picture,
 * upscaled from it, into out (row j at out + j * w). In the program's
 * address space (`root`, current). -> 0 */
int igfx_gpu_material(uint64_t root, uint32_t *samp, uint32_t *scratch, int bw, int bh,
                      uint32_t sat, uint32_t tint, uint32_t alpha, uint32_t *out, int w, int j0, int j1, int bleed) {
    uint64_t t0 = gtsc();
    if (!GOOD(GPU_BOX3) || !GOOD(GPU_SATINT) || !GOOD(GPU_UPSCALE) || bw < 2 || bh < 2 || w < 1 || j0 < 0 || j1 <= j0) return refused(__LINE__);
    uint64_t small = (uint64_t)bw * bh * 4, rows = (uint64_t)(j1 - j0) * w * 4;
    uint32_t *out0 = out + (uint64_t)j0 * w;
    uint32_t s = map_user(0, root, (uint64_t)samp, small), t = map_user(1, root, (uint64_t)scratch, small),
             o = map_user(2, root, (uint64_t)out0, rows);
    if (!s || !t || !o) return refused(__LINE__);
    clflush_range(samp, small);                      /* the CPU's samples to memory; */
    clflush_range(scratch, small);                   /* nothing old over what the GPU writes */
    clflush_range(out0, rows);
    gst_add(0, gtsc() - t0);
    gpu_begin();
    gpu_params_t p;
    memset(&p, 0, sizeof(p));
    p.dst_pitch = p.src_pitch = (uint32_t)bw;
    p.w = (uint32_t)bw;
    p.h = (uint32_t)bh;
    uint32_t gx = ((uint32_t)bw + 15) / 16, gy = (uint32_t)bh;
    for (int pass = 0; pass < 4; pass++) {           /* across, down, across, down (as material()) */
        p.src = pass & 1 ? t : s;
        p.dst = pass & 1 ? s : t;
        p.a = !(pass & 1);
        gpu_pass(GPU_BOX3, &p, gx, gy);
    }
    p.src = p.dst = s;
    p.a = sat; p.b = tint; p.c = alpha;
    gpu_pass(GPU_SATINT, &p, gx, gy);
    memset(&p, 0, sizeof(p));
    p.src = s;
    p.src_pitch = (uint32_t)bw;
    p.dst = o;
    p.dst_pitch = (uint32_t)w;
    p.w = (uint32_t)w;
    p.h = (uint32_t)(j1 - j0);
    p.a = (uint32_t)bw; p.b = (uint32_t)bh; p.c = (uint32_t)bleed; p.d = (uint32_t)j0;
    gpu_pass(GPU_UPSCALE, &p, ((uint32_t)w + 15) / 16, (uint32_t)(j1 - j0));
    if (gpu_run("material")) return refused(__LINE__);
    uint64_t t1 = gtsc();
    clflush_range(out0, rows);                       /* the CPU reads the GPU's picture, not old lines */
    gst_add(3, gtsc() - t1);
    gst_batch();
    return 0;
}

/* A fade: out (w x h, `pitch` pixels a row) = lerp(old (w x h, packed), out, f). */
int igfx_gpu_fade(uint64_t root, const uint32_t *old, uint32_t *out, int w, int h, int pitch, uint32_t f) {
    uint64_t t0 = gtsc();
    if (!GOOD(GPU_FADE) || w < 1 || h < 1 || pitch < w) return refused(__LINE__);
    uint64_t ob = (uint64_t)w * h * 4, db = ((uint64_t)(h - 1) * pitch + w) * 4;
    uint32_t s = map_user(0, root, (uint64_t)old, ob), d = map_user(1, root, (uint64_t)out, db);
    if (!s || !d) return refused(__LINE__);
    clflush_range(old, ob);
    clflush_range(out, db);
    gst_add(0, gtsc() - t0);
    gpu_begin();
    gpu_params_t p;
    memset(&p, 0, sizeof(p));
    p.src = s;
    p.src_pitch = (uint32_t)w;
    p.dst = d;
    p.dst_pitch = (uint32_t)pitch;
    p.w = (uint32_t)w;
    p.h = (uint32_t)h;
    p.a = f;
    gpu_pass(GPU_FADE, &p, ((uint32_t)w + 15) / 16, (uint32_t)h);
    if (gpu_run("fade")) return refused(__LINE__);
    uint64_t t1 = gtsc();
    clflush_range(out, db);
    gst_add(3, gtsc() - t1);
    gst_batch();
    return 0;
}

/* Drawings onto a program's canvas (gfx GFX_GPU_DRAW), in order, one batch.
 * The canvas's rows that the drawings touch go at VA_CANVAS; the pictures
 * they read into the slots. `ok` checks a program's range (ptr, bytes, write).
 * -> 0 done (all of them), -1: none done, the program draws them. */
#define VA_CANVAS  0x14000000u                 /* up to 192 MiB (to VA_BUF1) */
#define CANVAS_MAX 0x0C000000u

static int in16(int v) { return v > -32768 && v < 32768; }

int igfx_gpu_draw(uint64_t root, const struct os_gpu_draws *d, int (*ok)(uint64_t, uint64_t, int)) {
    uint64_t t0 = gtsc();
    static const int kernel_of[] = { 0, GPU_SHADOW, GPU_OVER, GPU_RCOPY, GPU_SCALED, GPU_RFILL };
    int n = d->n, st = d->stride;
    if (!gpu_ok || n < 1 || n > GD_MAX || st < 1 || st > 8192 || d->cx0 < 0 || d->cy0 < 0 || d->cx1 > st ||
        d->cy1 > 8192 || d->cx1 <= d->cx0 || d->cy1 <= d->cy0)
        return refused(__LINE__);
    int rx0[GD_MAX], ry0[GD_MAX], rx1[GD_MAX], ry1[GD_MAX];
    int ux0 = d->cx1, uy0 = d->cy1, ux1 = d->cx0, uy1 = d->cy0;   /* all the regions */
    for (int i = 0; i < n; i++) {
        const struct os_gpu_draw *o = &d->op[i];
        if (o->op < GD_SHADOW || o->op > GD_FILL || !GOOD(kernel_of[o->op])) return refused(__LINE__);
        if (!in16(o->x) || !in16(o->y) || o->w < 1 || o->h < 1 || o->w > 8192 || o->h > 8192 ||
            o->size < 0 || o->size > 1024 || o->r < 0 || o->r > 4096 || o->alpha < 0 || o->alpha > 255)
            return refused(__LINE__);
        int g = o->op == GD_SHADOW ? o->size : 0;
        rx0[i] = o->x - g > d->cx0 ? o->x - g : d->cx0;
        ry0[i] = o->y - g > d->cy0 ? o->y - g : d->cy0;
        rx1[i] = o->x + o->w + g < d->cx1 ? o->x + o->w + g : d->cx1;
        ry1[i] = o->y + o->h + g < d->cy1 ? o->y + o->h + g : d->cy1;
        if (rx1[i] <= rx0[i] || ry1[i] <= ry0[i]) continue;
        if (rx0[i] < ux0) ux0 = rx0[i];
        if (ry0[i] < uy0) uy0 = ry0[i];
        if (rx1[i] > ux1) ux1 = rx1[i];
        if (ry1[i] > uy1) uy1 = ry1[i];
    }
    if (ux1 <= ux0) return 0;                        /* nothing of them is in the clip */
    uint32_t *cbase = d->px + (uint64_t)uy0 * st + ux0;
    uint64_t cbytes = ((uint64_t)(uy1 - uy0 - 1) * st + (ux1 - ux0)) * 4;
    if (!ok((uint64_t)cbase, cbytes, 1)) return refused(__LINE__);
    uint32_t cva = map_at(VA_CANVAS, CANVAS_MAX, root, (uint64_t)cbase, cbytes);
    if (!cva) return refused(__LINE__);
    gpu_params_t ps[GD_MAX];
    int slot = 0;
    for (int i = 0; i < n; i++) {                    /* the pictures: checked, mapped, to memory */
        const struct os_gpu_draw *o = &d->op[i];
        int w = rx1[i] - rx0[i], h = ry1[i] - ry0[i];
        if (w <= 0 || h <= 0) continue;
        gpu_params_t *p = &ps[i];
        memset(p, 0, sizeof(*p));
        p->dst = cva + (uint32_t)(((uint64_t)(ry0[i] - uy0) * st + (rx0[i] - ux0)) * 4);
        p->dst_pitch = (uint32_t)st;
        p->w = (uint32_t)w;
        p->h = (uint32_t)h;
        p->a = (uint32_t)(o->x - rx0[i]);
        p->b = (uint32_t)(o->y - ry0[i]);
        int keep = o->flags & GD_KEEP;
        if (o->op == GD_SHADOW) {
            p->c = (uint32_t)o->w;
            p->d = (uint32_t)o->h;
            p->e = (uint32_t)o->r | (uint32_t)o->size << 16;
            p->f = (uint32_t)o->alpha | (uint32_t)(o->flags & 1) << 16;
        } else if (o->op == GD_FILL) {
            int r = o->r;
            if (r * 2 > o->h) r = o->h / 2;
            if (r * 2 > o->w) r = o->w / 2;
            p->c = (uint32_t)o->w;
            p->d = (uint32_t)o->h;
            p->e = (uint32_t)r;
            p->f = (uint32_t)(o->flags & 15) | (uint32_t)o->alpha << 8;
            p->src = o->color & 0xFFFFFF;
        } else if (o->op == GD_OVER || o->op == GD_ROUND) {
            int ss = o->src_stride;
            if (ss < o->w || ss > 8192 || slot >= 8) return refused(__LINE__);
            const uint32_t *s = o->src + (uint64_t)(ry0[i] - o->y) * ss + (rx0[i] - o->x);
            uint64_t sb = ((uint64_t)(h - 1) * ss + w) * 4;
            if (!ok((uint64_t)s, sb, 0)) return refused(__LINE__);
            uint32_t sva = map_user(slot++, root, (uint64_t)s, sb);
            if (!sva) return refused(__LINE__);
            if (!keep) clflush_rect(s, ss, w, h);
            p->src = sva;
            p->src_pitch = (uint32_t)ss;
            if (o->op == GD_OVER) {                  /* the hole, from the region's corner */
                p->a = (uint32_t)(o->hx0 - rx0[i]);
                p->b = (uint32_t)(o->hy0 - ry0[i]);
                p->c = (uint32_t)(o->hx1 - rx0[i]);
                p->d = (uint32_t)(o->hy1 - ry0[i]);
                if (!in16(o->hx0) || !in16(o->hy0) || !in16(o->hx1) || !in16(o->hy1)) return refused(__LINE__);
            } else {
                int r = o->r;
                if (r * 2 > o->h) r = o->h / 2;
                if (r * 2 > o->w) r = o->w / 2;
                p->c = (uint32_t)o->w;
                p->d = (uint32_t)o->h;
                p->e = (uint32_t)r;
                p->f = (uint32_t)(o->flags & 15);
            }
        } else {                                     /* GD_SCALED */
            int ow = o->src_w, oh = o->src_h;
            if (ow < 1 || oh < 1 || ow > 8192 || oh > 8192 || o->w > 65535 || o->h > 65535 || slot + 3 > 8) return refused(__LINE__);
            uint64_t pb = (uint64_t)ow * oh * 4, ub = (uint64_t)w * h * 4;
            if (!ok((uint64_t)o->src, pb, 0) || !ok((uint64_t)o->mask, pb, 0)) return refused(__LINE__);
            uint32_t sva = map_user(slot++, root, (uint64_t)o->src, pb);
            uint32_t mva = map_user(slot++, root, (uint64_t)o->mask, pb), uva = 0;
            if (!sva || !mva) return refused(__LINE__);
            if (o->under) {
                if (!ok((uint64_t)o->under, ub, 0) || !(uva = map_user(slot++, root, (uint64_t)o->under, ub))) return refused(__LINE__);
                clflush_range(o->under, ub);
            }
            if (!keep) {
                clflush_range(o->src, pb);
                clflush_range(o->mask, pb);
            }
            p->src = sva;
            p->src_pitch = (uint32_t)ow;
            p->c = (uint32_t)o->w | (uint32_t)o->h << 16;
            p->d = (uint32_t)oh | (uint32_t)o->alpha << 16;
            p->e = mva;
            p->f = uva;
        }
    }
    clflush_rect(cbase, st, ux1 - ux0, uy1 - uy0);   /* what the CPU drew there, to memory */
    gst_add(0, gtsc() - t0);
    gpu_begin();
    for (int i = 0; i < n; i++)
        if (rx1[i] > rx0[i] && ry1[i] > ry0[i])
            gpu_pass(kernel_of[d->op[i].op], &ps[i], ps[i].w / 16 + (ps[i].w % 16 != 0), ps[i].h);
    if (gpu_run("draw")) return refused(__LINE__);
    uint64_t t1 = gtsc();
    clflush_rect(cbase, st, ux1 - ux0, uy1 - uy0);   /* and nothing old over what the GPU wrote */
    gst_add(3, gtsc() - t1);
    gst_batch();
    return 0;
}
