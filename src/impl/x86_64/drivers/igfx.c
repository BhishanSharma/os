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
