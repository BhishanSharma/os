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

static volatile uint8_t *regs;          /* the register BAR, uncached */
static uint64_t gsm_phys;               /* the GGTT's page table: BAR 0 + 8 MiB */
static int pipe = -1;
static uint32_t surf0;                  /* the firmware framebuffer's GGTT address */
static uint32_t surf_low;               /* low bits of PLANE_SURF to keep */
static uint32_t width, height, pitch;
static int ok, cursor_on, cursor_hx, cursor_hy;
static uint32_t *cursor_va, *back_va;
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

uint32_t *igfx_back_buffer(void) {
    if (!ok) return 0;
    if (!back_va) back_va = gpu_buffer(GGTT_BACK, (pitch * height + 4095) / 4096);
    return back_va;
}

void igfx_flip(int buffer) {
    if (!ok) return;
    flipped_to = buffer ? GGTT_BACK : surf0;
    __asm__ volatile("sfence" ::: "memory");
    wr(PLANE_SURF, flipped_to | surf_low);              /* taken at the next refresh */
}

int igfx_flip_done(void) {
    return !ok || (rd(PLANE_SURFLIVE) & ~0xFFFu) == flipped_to;
}
