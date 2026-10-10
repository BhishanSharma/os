// xhci.c - USB 3 host controller (xHCI) and USB mice
//
// The controller works from rings of 16-byte TRBs in memory: a command ring
// (we ask it to give a device an address, set up an endpoint, ...), one
// transfer ring per endpoint (data to and from a device), and an event ring
// where it reports what finished. A doorbell register tells it to look at a
// ring. Everything here polls the event ring (no interrupt is needed): during
// setup by waiting for each answer, afterwards from the timer interrupt.
//
// Reference: the eXtensible Host Controller Interface specification 1.2, and
// the USB 2.0 spec chapter 9 (device requests) and HID 1.11 appendix B (boot
// protocol mouse).
#include "drivers/usb.h"
#include "drivers/pci.h"
#include "drivers/paging.h"
#include "drivers/heap.h"
#include "drivers/timer.h"
#include "drivers/mouse.h"
#include "drivers/keyboard.h"
#include "lib/print.h"
#include "lib/string.h"
#include <stdint.h>

#define PAGE_PCD 0x10
#define PAGE_PWT 0x08

/* ---- Registers -------------------------------------------------------------- */

/* Capability registers (offsets from the BAR) */
#define CAP_LENGTH     0x00
#define CAP_HCSPARAMS1 0x04
#define CAP_HCSPARAMS2 0x08
#define CAP_HCCPARAMS1 0x10
#define CAP_DBOFF      0x14
#define CAP_RTSOFF     0x18

/* Operational registers (offsets from BAR + CAPLENGTH) */
#define OP_USBCMD   0x00
#define OP_USBSTS   0x04
#define OP_PAGESIZE 0x08
#define OP_CRCR     0x18
#define OP_DCBAAP   0x30
#define OP_CONFIG   0x38
#define OP_PORTSC   0x400            /* + 0x10 per port */

#define CMD_RUN     (1u << 0)
#define CMD_RESET   (1u << 1)
#define STS_HALTED  (1u << 0)
#define STS_NOT_READY (1u << 11)

/* PORTSC bits */
#define PORT_CONNECT  (1u << 0)
#define PORT_ENABLED  (1u << 1)
#define PORT_RESET    (1u << 4)
#define PORT_POWER    (1u << 9)
#define PORT_SPEED(v) (((v) >> 10) & 0xF)
#define PORT_CHANGES  (0x7Fu << 17)  /* write 1 to clear */
#define PORT_RESET_CHANGE (1u << 21)
/* Bits that keep their value when written back (the rest are write-1-to-
 * clear or have side effects): see xHCI 5.4.8. */
#define PORT_KEEP     0x0E01C3E0u

/* Interrupter 0 (offsets from BAR + RTSOFF) */
#define IR0_ERSTSZ  0x28
#define IR0_ERSTBA  0x30
#define IR0_ERDP    0x38

/* Speeds (PORTSC and slot context) */
#define SPEED_FULL  1
#define SPEED_LOW   2
#define SPEED_HIGH  3
#define SPEED_SUPER 4

/* ---- TRBs ------------------------------------------------------------------ */

typedef struct {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) trb_t;

#define TRB_CYCLE      (1u << 0)
#define TRB_TOGGLE     (1u << 1)     /* link TRB: flip the cycle bit */
#define TRB_ISP        (1u << 2)     /* interrupt on short packet */
#define TRB_IOC        (1u << 5)     /* make an event when done */
#define TRB_IDT        (1u << 6)     /* data inside the TRB (setup stage) */
#define TRB_DIR_IN     (1u << 16)
#define TRB_TYPE(t)    ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c) (((c) >> 10) & 0x3F)

enum {
    TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_LINK = 6,
    TRB_ENABLE_SLOT = 9, TRB_ADDRESS_DEVICE = 11, TRB_CONFIGURE_EP = 12, TRB_EVALUATE = 13,
    TRB_EV_TRANSFER = 32, TRB_EV_COMMAND = 33, TRB_EV_PORT = 34,
};

#define CC_SUCCESS     1
#define CC_SHORT       13

#define RING_TRBS 64

typedef struct {
    trb_t *trbs;
    int enqueue;
    uint32_t cycle;
} ring_t;

/* ---- State ----------------------------------------------------------------- */

#define MAX_DEVICES 16
#define HID_KEYBOARD 1              /* = the boot interface protocol numbers */
#define HID_MOUSE    2

/* One boot-protocol HID interface (a device such as a wireless receiver can
 * have a keyboard and a mouse) and its interrupt IN endpoint. */
#define MAX_HID 3
typedef struct {
    int kind;                       /* HID_MOUSE or HID_KEYBOARD */
    uint8_t iface;
    int dci;                        /* device context index of the endpoint */
    int len;
    ring_t ring;
    uint8_t *report;                /* where the controller writes a report */
    uint8_t last[8];                /* keyboard: the previous report */
} hid_t;

typedef struct {
    int port;                       /* 1-based root port */
    int speed;
    int slot;
    uint16_t vendor, product;
    uint8_t dev_class, if_class, if_subclass, if_protocol;
    hid_t hid[MAX_HID];
    int hid_count;
    const char *note;               /* why it is not used, if it is not */
    ring_t ep0;
    void *in_ctx, *out_ctx;
} usb_device_t;

/* Keyboard repeat (USB keyboards only say what is held down). */
static uint8_t repeat_code;
static uint32_t repeat_tick;

static volatile uint8_t *mmio;
static volatile uint8_t *op;
static volatile uint8_t *rt;
static volatile uint32_t *doorbells;
static int ctx_size = 32;           /* 64 if HCCPARAMS1.CSZ */
static int max_slots, max_ports;
static uint64_t *dcbaa;
static ring_t cmd_ring;
static trb_t *event_ring;
static int event_dequeue;
static uint32_t event_cycle = 1;
static usb_device_t devices[MAX_DEVICES];
static int device_count, mouse_count, keyboard_count;
static int found;
static uint16_t ctrl_vendor, ctrl_device;
static char description[96];
static volatile int busy;           /* setup is waiting on the event ring */
static volatile int rescan_wanted;

static uint32_t r32(volatile uint8_t *base, uint32_t off) { return *(volatile uint32_t *)(base + off); }
static void w32(volatile uint8_t *base, uint32_t off, uint32_t v) { *(volatile uint32_t *)(base + off) = v; }
static void w64(volatile uint8_t *base, uint32_t off, uint64_t v) {
    w32(base, off, (uint32_t)v);
    w32(base, off + 4, (uint32_t)(v >> 32));
}

static void map_mmio(uint64_t base, uint64_t size) {
    for (uint64_t a = base & ~0xFFFull; a < base + size; a += 0x1000)
        map_page(a, a, PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);
}

/* Zeroed memory with the given alignment (the heap is identity-mapped, so
 * the address is also what the controller uses). Never freed. */
static void *dma_alloc(uint64_t size, uint64_t align) {
    uint8_t *raw = kmalloc(size + align);
    if (!raw) return 0;
    uint8_t *p = (uint8_t *)(((uint64_t)raw + align - 1) & ~(align - 1));
    memset(p, 0, size);
    return p;
}

static void delay_ms(uint32_t ms) {
    uint32_t start = get_tick(), ticks = (ms * TIMER_FREQ + 999) / 1000;
    while ((uint32_t)(get_tick() - start) < ticks) __asm__ volatile("hlt");
}

/* ---- Rings ----------------------------------------------------------------- */

static int ring_init(ring_t *r) {
    r->trbs = dma_alloc(RING_TRBS * sizeof(trb_t), 4096);
    if (!r->trbs) return -1;
    r->enqueue = 0;
    r->cycle = 1;
    trb_t *link = &r->trbs[RING_TRBS - 1];
    link->param = (uint64_t)r->trbs;
    link->control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE;
    return 0;
}

/* Put a TRB on a ring (the cycle bit is set here); returns where it went. */
static trb_t *ring_push(ring_t *r, uint64_t param, uint32_t status, uint32_t control) {
    trb_t *t = &r->trbs[r->enqueue];
    t->param = param;
    t->status = status;
    __asm__ volatile("" ::: "memory");
    t->control = (control & ~TRB_CYCLE) | r->cycle;
    if (++r->enqueue == RING_TRBS - 1) {          /* reached the link TRB: hand it over */
        trb_t *link = &r->trbs[RING_TRBS - 1];
        link->control = (link->control & ~TRB_CYCLE) | r->cycle;
        r->enqueue = 0;
        r->cycle ^= 1;
    }
    return t;
}

static void ring_doorbell(int slot, int target) {
    __asm__ volatile("mfence" ::: "memory");
    doorbells[slot] = (uint32_t)target;
}

/* The next event, if there is one. */
static int event_pop(trb_t *out) {
    trb_t *e = &event_ring[event_dequeue];
    if ((e->control & TRB_CYCLE) != event_cycle) return 0;
    *out = *e;
    if (++event_dequeue == RING_TRBS) {
        event_dequeue = 0;
        event_cycle ^= 1;
    }
    w64(rt, IR0_ERDP, (uint64_t)&event_ring[event_dequeue] | (1u << 3));   /* EHB: handled */
    return 1;
}

static void handle_transfer(const trb_t *e);

/* Wait for the event that answers `trb` (a command or the last TRB of a
 * transfer). Other events met on the way are handled normally. Returns the
 * completion code, or -1 after `ms` milliseconds. */
static int wait_for(trb_t *trb, uint32_t ms, trb_t *event_out) {
    uint32_t start = get_tick(), ticks = (ms * TIMER_FREQ + 999) / 1000 + 1;
    for (;;) {
        trb_t e;
        while (event_pop(&e)) {
            uint32_t type = TRB_GET_TYPE(e.control);
            if ((type == TRB_EV_COMMAND || type == TRB_EV_TRANSFER) && e.param == (uint64_t)trb) {
                if (event_out) *event_out = e;
                return (int)(e.status >> 24);
            }
            if (type == TRB_EV_TRANSFER) handle_transfer(&e);
            if (type == TRB_EV_PORT) rescan_wanted = 1;
        }
        if ((uint32_t)(get_tick() - start) >= ticks) return -1;
        __asm__ volatile("pause");
    }
}

static int command(uint64_t param, uint32_t status, uint32_t control, trb_t *event_out) {
    trb_t *t = ring_push(&cmd_ring, param, status, control);
    ring_doorbell(0, 0);
    return wait_for(t, 500, event_out);
}

/* ---- Contexts -------------------------------------------------------------- */

/* Input context: entry 0 is the input control context, 1 the slot, 2 EP0,
 * n+1 endpoint n (DCI n). Output (device) context: 0 slot, n endpoint n. */
static uint32_t *in_entry(usb_device_t *d, int i) { return (uint32_t *)((uint8_t *)d->in_ctx + i * ctx_size); }

static int ep0_max_packet(int speed) {
    switch (speed) {
    case SPEED_SUPER: return 512;
    case SPEED_HIGH:  return 64;
    default:          return 8;
    }
}

/* ---- Control transfers ----------------------------------------------------- */

static int control(usb_device_t *d, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                   void *data, uint16_t length) {
    uint64_t setup = type | ((uint64_t)request << 8) | ((uint64_t)value << 16) |
                     ((uint64_t)index << 32) | ((uint64_t)length << 48);
    int in = (type & 0x80) != 0;
    uint32_t trt = length ? (in ? 3u : 2u) : 0u;
    ring_push(&d->ep0, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));
    if (length)
        ring_push(&d->ep0, (uint64_t)data, length, TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN : 0));
    trb_t *status = ring_push(&d->ep0, 0, 0,
                              TRB_TYPE(TRB_STATUS) | TRB_IOC | ((length && in) ? 0 : TRB_DIR_IN));
    ring_doorbell(d->slot, 1);
    int cc = wait_for(status, 1000, 0);
    return cc == CC_SUCCESS ? 0 : -1;
}

static int get_descriptor(usb_device_t *d, uint8_t type, uint8_t index, void *buf, uint16_t len) {
    return control(d, 0x80, 6, (uint16_t)((type << 8) | index), 0, buf, len);
}

/* ---- Device setup ---------------------------------------------------------- */

/* Reset a root port; returns its speed, or 0 if nothing is there. */
static int port_reset(int port) {
    uint32_t off = OP_PORTSC + 0x10 * (uint32_t)(port - 1);
    uint32_t v = r32(op, off);
    if (!(v & PORT_CONNECT)) return 0;
    w32(op, off, (v & PORT_KEEP) | PORT_RESET);
    for (int i = 0; i < 50; i++) {
        delay_ms(10);
        v = r32(op, off);
        if ((v & PORT_RESET_CHANGE) && !(v & PORT_RESET)) break;
    }
    w32(op, off, (v & PORT_KEEP) | PORT_CHANGES);           /* clear the change bits */
    delay_ms(20);                                           /* recovery time */
    v = r32(op, off);
    if (!(v & PORT_ENABLED)) return 0;
    return (int)PORT_SPEED(v);
}

static int address_device(usb_device_t *d) {
    trb_t ev;
    if (command(0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &ev) != CC_SUCCESS) {
        d->note = "controller gave no slot";
        return -1;
    }
    d->slot = (int)(ev.control >> 24);
    if (d->slot < 1 || d->slot > max_slots) { d->note = "bad slot"; return -1; }

    d->out_ctx = dma_alloc((uint64_t)ctx_size * 32, 64);
    d->in_ctx = dma_alloc((uint64_t)ctx_size * 33, 64);
    if (!d->out_ctx || !d->in_ctx || ring_init(&d->ep0) != 0) { d->note = "out of memory"; return -1; }
    dcbaa[d->slot] = (uint64_t)d->out_ctx;

    uint32_t *icc = in_entry(d, 0), *slot = in_entry(d, 1), *ep0 = in_entry(d, 2);
    icc[1] = 0x3;                                           /* add: slot, EP0 */
    slot[0] = ((uint32_t)d->speed << 20) | (1u << 27);      /* one context entry */
    slot[1] = (uint32_t)d->port << 16;                      /* root hub port */
    ep0[1] = (3u << 1) | (4u << 3) | ((uint32_t)ep0_max_packet(d->speed) << 16);   /* CErr 3, control */
    uint64_t deq = (uint64_t)d->ep0.trbs | 1;
    ep0[2] = (uint32_t)deq;
    ep0[3] = (uint32_t)(deq >> 32);
    ep0[4] = 8;                                             /* average TRB length */

    int cc = command((uint64_t)d->in_ctx, 0, TRB_TYPE(TRB_ADDRESS_DEVICE) | ((uint32_t)d->slot << 24), 0);
    if (cc != CC_SUCCESS) { d->note = "did not take an address"; return -1; }
    return 0;
}

/* Full- and low-speed devices: the first 8 bytes of the device descriptor
 * say how big EP0's packets really are; tell the controller if not 8. */
static int fix_ep0_size(usb_device_t *d, uint8_t *buf) {
    if (get_descriptor(d, 1, 0, buf, 8) != 0) return -1;
    int mps = buf[7];
    if (d->speed == SPEED_SUPER) mps = 1 << mps;
    if (mps == ep0_max_packet(d->speed) || mps < 8) return 0;
    uint32_t *icc = in_entry(d, 0), *ep0 = in_entry(d, 2);
    icc[0] = 0;
    icc[1] = 0x2;                                           /* evaluate EP0 */
    ep0[1] = (ep0[1] & 0xFFFF) | ((uint32_t)mps << 16);
    return command((uint64_t)d->in_ctx, 0, TRB_TYPE(TRB_EVALUATE) | ((uint32_t)d->slot << 24), 0)
           == CC_SUCCESS ? 0 : -1;
}

/* xHCI wants the polling interval as 2^n * 125 us. */
static uint32_t xhci_interval(int speed, uint8_t b_interval) {
    if (speed == SPEED_HIGH || speed == SPEED_SUPER) {
        int n = b_interval ? b_interval - 1 : 0;
        return (uint32_t)(n > 15 ? 15 : n);
    }
    uint32_t frames = b_interval ? b_interval : 1, n = 3;   /* 1 ms = 2^3 * 125 us */
    while (n < 10 && (1u << (n + 1)) <= frames * 8) n++;
    return n;
}

static void queue_report(usb_device_t *d, hid_t *h) {
    ring_push(&h->ring, (uint64_t)h->report, (uint32_t)h->len, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    ring_doorbell(d->slot, h->dci);
}

/* Set up the endpoints of the HID interfaces found, select the
 * configuration, and switch each interface to the boot protocol. */
static int setup_hid(usb_device_t *d, uint8_t config_value, const uint16_t *mps, const uint8_t *b_interval) {
    uint32_t *icc = in_entry(d, 0), *slot = in_entry(d, 1);
    memset(d->in_ctx, 0, (uint64_t)ctx_size * 33);
    memcpy(slot, d->out_ctx, (uint64_t)ctx_size);           /* current slot context */
    icc[1] = 1u;                                            /* add: slot */
    int last_dci = 1;
    for (int i = 0; i < d->hid_count; i++) {
        hid_t *h = &d->hid[i];
        h->len = mps[i] > 64 ? 64 : mps[i];
        h->report = dma_alloc(64, 64);
        if (!h->report || ring_init(&h->ring) != 0) { d->note = "out of memory"; return -1; }
        uint32_t *ep = in_entry(d, h->dci + 1);
        icc[1] |= 1u << h->dci;
        if (h->dci > last_dci) last_dci = h->dci;
        ep[0] = xhci_interval(d->speed, b_interval[i]) << 16;
        ep[1] = (3u << 1) | (7u << 3) | ((uint32_t)mps[i] << 16);   /* CErr 3, interrupt IN */
        uint64_t deq = (uint64_t)h->ring.trbs | 1;
        ep[2] = (uint32_t)deq;
        ep[3] = (uint32_t)(deq >> 32);
        ep[4] = (uint32_t)mps[i] | ((uint32_t)mps[i] << 16);   /* average TRB length, max ESIT payload */
    }
    slot[0] = (slot[0] & ~(0x1Fu << 27)) | ((uint32_t)last_dci << 27);

    if (command((uint64_t)d->in_ctx, 0, TRB_TYPE(TRB_CONFIGURE_EP) | ((uint32_t)d->slot << 24), 0)
        != CC_SUCCESS) { d->note = "endpoint setup failed"; return -1; }
    if (control(d, 0x00, 9, config_value, 0, 0, 0) != 0) { d->note = "SET_CONFIGURATION failed"; return -1; }
    for (int i = 0; i < d->hid_count; i++) {
        hid_t *h = &d->hid[i];
        control(d, 0x21, 0x0B, 0, h->iface, 0, 0);          /* SET_PROTOCOL: boot (may be refused) */
        control(d, 0x21, 0x0A, 0, h->iface, 0, 0);          /* SET_IDLE: report only on change */
        if (h->kind == HID_MOUSE) mouse_count++;
        else keyboard_count++;
        queue_report(d, h);
    }
    return 0;
}

static void setup_device(usb_device_t *d) {
    uint8_t *buf = dma_alloc(512, 64);
    if (!buf) { d->note = "out of memory"; return; }
    if (address_device(d) != 0) return;
    if (fix_ep0_size(d, buf) != 0) { d->note = "no device descriptor"; return; }
    if (get_descriptor(d, 1, 0, buf, 18) != 0) { d->note = "no device descriptor"; return; }
    d->dev_class = buf[4];
    d->vendor = (uint16_t)(buf[8] | buf[9] << 8);
    d->product = (uint16_t)(buf[10] | buf[11] << 8);

    if (get_descriptor(d, 2, 0, buf, 9) != 0) { d->note = "no configuration"; return; }
    uint16_t total = (uint16_t)(buf[2] | buf[3] << 8);
    if (total > 512) total = 512;
    if (get_descriptor(d, 2, 0, buf, total) != 0) { d->note = "no configuration"; return; }
    uint8_t config_value = buf[5];

    /* Walk the descriptors: the first interface (for lsusb), and each boot
     * keyboard or mouse interface with its interrupt IN endpoint. */
    int kind = 0, have_iface = 0;
    uint8_t iface = 0;
    uint16_t mps[MAX_HID];
    uint8_t interval[MAX_HID];
    for (int i = 0; i + 1 < total && buf[i] >= 2; i += buf[i]) {
        uint8_t len = buf[i], type = buf[i + 1];
        if (type == 4 && len >= 9) {
            if (!have_iface) {
                d->if_class = buf[i + 5];
                d->if_subclass = buf[i + 6];
                d->if_protocol = buf[i + 7];
                have_iface = 1;
            }
            int boot = buf[i + 5] == 3 && buf[i + 6] == 1;
            kind = boot && (buf[i + 7] == HID_MOUSE || buf[i + 7] == HID_KEYBOARD) ? buf[i + 7] : 0;
            iface = buf[i + 2];
        } else if (type == 5 && len >= 7 && kind && (buf[i + 2] & 0x80) && (buf[i + 3] & 3) == 3 &&
                   d->hid_count < MAX_HID) {
            hid_t *h = &d->hid[d->hid_count];
            h->kind = kind;
            h->iface = iface;
            h->dci = (buf[i + 2] & 0xF) * 2 + 1;
            mps[d->hid_count] = (uint16_t)((buf[i + 4] | buf[i + 5] << 8) & 0x7FF);
            interval[d->hid_count] = buf[i + 6];
            d->hid_count++;
            kind = 0;                                       /* one endpoint per interface */
        }
    }
    if (d->hid_count) {
        if (setup_hid(d, config_value, mps, interval) != 0) d->hid_count = 0;
        return;
    }
    if (d->dev_class == 9 || d->if_class == 9) d->note = "hub: devices behind hubs are not supported yet";
    else d->note = "no driver";
}

static int device_kinds(const usb_device_t *d) {
    int k = 0;
    for (int i = 0; i < d->hid_count; i++) k |= 1 << d->hid[i].kind;
    return k;
}

static void scan_ports(void) {
    for (int port = 1; port <= max_ports && device_count < MAX_DEVICES; port++) {
        int known = 0;
        for (int i = 0; i < device_count; i++)
            if (devices[i].port == port) known = 1;
        if (known) continue;
        uint32_t v = r32(op, OP_PORTSC + 0x10 * (uint32_t)(port - 1));
        if (!(v & PORT_CONNECT)) continue;
        usb_device_t *d = &devices[device_count];
        memset(d, 0, sizeof(*d));
        d->port = port;
        d->speed = port_reset(port);
        if (!d->speed) continue;                            /* USB 3 half of a USB 2 device, etc. */
        device_count++;
        setup_device(d);
        kprintf("USB port %d: %04x:%04x class %u/%u/%u%s%s%s%s\n", port, d->vendor, d->product,
                d->if_class, d->if_subclass, d->if_protocol,
                (device_kinds(d) >> HID_KEYBOARD) & 1 ? ", keyboard" : "",
                (device_kinds(d) >> HID_MOUSE) & 1 ? ", mouse" : "",
                d->note ? ": " : "", d->note ? d->note : "");
    }
}

/* ---- Reports --------------------------------------------------------------- */

/* USB key codes (HID usage page 7) to PS/2 scan code set 1; 0x80 set =
 * extended (E0 prefix). 0 = not translated. */
static const uint8_t usage_to_set1[0x53] = {
    0, 0, 0, 0,
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26,   /* a-l */
    0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D,   /* m-x */
    0x15, 0x2C,                                                               /* y z */
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,               /* 1-0 */
    0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B,               /* Enter Esc BS Tab Space - = [ ] \ */
    0x2B, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35, 0x3A,                           /* # ; ' ` , . / Caps */
    0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,   /* F1-F12 */
    0, 0x46, 0,                                                               /* PrtSc ScrollLock Pause */
    0xD2, 0xC7, 0xC9, 0xD3, 0xCF, 0xD1,                                       /* Ins Home PgUp Del End PgDn */
    0xCD, 0xCB, 0xD0, 0xC8,                                                   /* Right Left Down Up */
};

/* Modifier bits of byte 0: LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI. */
static const uint8_t modifier_set1[8] = { 0x1D, 0x2A, 0x38, 0xDB, 0x9D, 0x36, 0xB8, 0xDC };

static void send_set1(uint8_t code, int release) {
    if (code & 0x80) keyboard_scancode(0xE0);
    keyboard_scancode((uint8_t)((code & 0x7F) | (release ? 0x80 : 0)));
}

static int report_has(const uint8_t *r, uint8_t usage) {
    for (int i = 2; i < 8; i++)
        if (r[i] == usage) return 1;
    return 0;
}

static void keyboard_report(hid_t *h, const uint8_t *r) {
    if (r[2] == 1) return;                                  /* "too many keys": keep the old state */
    uint8_t *old = h->last;
    for (int b = 0; b < 8; b++) {
        int was = (old[0] >> b) & 1, now = (r[0] >> b) & 1;
        if (was != now) send_set1(modifier_set1[b], !now);
    }
    for (int i = 2; i < 8; i++)                             /* released */
        if (old[i] >= 4 && old[i] < 0x53 && !report_has(r, old[i]) && usage_to_set1[old[i]]) {
            send_set1(usage_to_set1[old[i]], 1);
            if (repeat_code == old[i]) repeat_code = 0;
        }
    for (int i = 2; i < 8; i++)                             /* pressed */
        if (r[i] >= 4 && r[i] < 0x53 && !report_has(old, r[i]) && usage_to_set1[r[i]]) {
            send_set1(usage_to_set1[r[i]], 0);
            repeat_code = r[i];
            repeat_tick = get_tick() + TIMER_FREQ / 2;      /* repeat after 500 ms */
        }
    memcpy(old, r, 8);
}

static void handle_transfer(const trb_t *e) {
    int slot = (int)(e->control >> 24), dci = (int)((e->control >> 16) & 0x1F);
    for (int i = 0; i < device_count; i++) {
        usb_device_t *d = &devices[i];
        if (d->slot != slot) continue;
        for (int j = 0; j < d->hid_count; j++) {
            hid_t *h = &d->hid[j];
            if (h->dci != dci) continue;
            int cc = (int)(e->status >> 24);
            int got = h->len - (int)(e->status & 0xFFFFFF);
            if (cc == CC_SUCCESS || cc == CC_SHORT) {
                const uint8_t *r = h->report;
                if (h->kind == HID_MOUSE && got >= 3)
                    mouse_report("USB", r[0] & 7, (int8_t)r[1], (int8_t)r[2], got >= 4 ? (int8_t)r[3] : 0);
                else if (h->kind == HID_KEYBOARD && got >= 8)
                    keyboard_report(h, r);
            }
            queue_report(d, h);
            return;
        }
    }
}

/* Timer interrupt: collect reports, repeat a held key. */
static void usb_tick(void) {
    if (busy || !found) return;
    trb_t e;
    while (event_pop(&e)) {
        uint32_t type = TRB_GET_TYPE(e.control);
        if (type == TRB_EV_TRANSFER) handle_transfer(&e);
        else if (type == TRB_EV_PORT) rescan_wanted = 1;
    }
    if (repeat_code && (int32_t)(get_tick() - repeat_tick) >= 0) {
        send_set1(usage_to_set1[repeat_code], 0);          /* a second press, as PS/2 keyboards send */
        repeat_tick = get_tick() + TIMER_FREQ / 20;         /* then 20 a second */
    }
}

/* ---- Controller ------------------------------------------------------------ */

/* The firmware may be using the controller (for its own USB keyboard
 * support); ask it to let go. */
static void take_from_bios(uint64_t base, uint32_t hccparams1) {
    uint32_t off = (hccparams1 >> 16) << 2;
    for (int guard = 0; off && guard < 64; guard++) {
        map_mmio(base + off, 8);
        volatile uint32_t *cap = (volatile uint32_t *)(base + off);
        uint32_t v = cap[0];
        if ((v & 0xFF) == 1) {                              /* USB legacy support */
            if (v & (1u << 16)) {
                cap[0] = v | (1u << 24);                    /* OS owned */
                for (int i = 0; i < 100 && (cap[0] & (1u << 16)); i++) delay_ms(10);
                kprintf("xHCI: firmware %s the controller\n",
                        (cap[0] & (1u << 16)) ? "did not release" : "released");
            }
            cap[1] = (cap[1] & ~0x1F) | 0xE0000000u;           /* SMIs off, clear their status */
            return;
        }
        uint32_t next = (v >> 8) & 0xFF;
        if (!next) break;
        off += next << 2;
    }
}

static int controller_start(uint64_t base) {
    map_mmio(base, 0x1000);
    mmio = (volatile uint8_t *)base;
    uint32_t caplen = mmio[CAP_LENGTH];
    uint32_t hcs1 = r32(mmio, CAP_HCSPARAMS1), hcs2 = r32(mmio, CAP_HCSPARAMS2);
    uint32_t hcc1 = r32(mmio, CAP_HCCPARAMS1);
    uint32_t dboff = r32(mmio, CAP_DBOFF) & ~3u, rtsoff = r32(mmio, CAP_RTSOFF) & ~0x1Fu;
    max_slots = (int)(hcs1 & 0xFF);
    max_ports = (int)(hcs1 >> 24);
    if (hcc1 & (1u << 2)) ctx_size = 64;
    if (max_slots > MAX_DEVICES) max_slots = MAX_DEVICES;

    map_mmio(base, caplen + OP_PORTSC + 0x10 * (uint64_t)max_ports);
    map_mmio(base + dboff, 4 * 256);
    map_mmio(base + rtsoff, 0x40);
    op = mmio + caplen;
    rt = mmio + rtsoff;
    doorbells = (volatile uint32_t *)(mmio + dboff);

    take_from_bios(base, hcc1);

    /* Stop, then reset. */
    w32(op, OP_USBCMD, r32(op, OP_USBCMD) & ~CMD_RUN);
    for (int i = 0; i < 100 && !(r32(op, OP_USBSTS) & STS_HALTED); i++) delay_ms(1);
    w32(op, OP_USBCMD, CMD_RESET);
    for (int i = 0; i < 100 && (r32(op, OP_USBCMD) & CMD_RESET); i++) delay_ms(10);
    for (int i = 0; i < 100 && (r32(op, OP_USBSTS) & STS_NOT_READY); i++) delay_ms(10);
    if (r32(op, OP_USBCMD) & CMD_RESET) { kprintf("xHCI: reset did not finish\n"); return -1; }

    /* Device context table, with scratchpad pages if the controller wants them. */
    dcbaa = dma_alloc(8 * 256, 64);
    if (!dcbaa) return -1;
    uint32_t scratch = ((hcs2 >> 21) & 0x1F) << 5 | (hcs2 >> 27);
    if (scratch) {
        uint32_t page = (r32(op, OP_PAGESIZE) & 0xFFFF) << 12;
        if (!page) page = 4096;
        uint64_t *list = dma_alloc(8 * (uint64_t)scratch, 64);
        if (!list) return -1;
        for (uint32_t i = 0; i < scratch; i++) {
            void *p = dma_alloc(page, page);
            if (!p) return -1;
            list[i] = (uint64_t)p;
        }
        dcbaa[0] = (uint64_t)list;
    }
    w64(op, OP_DCBAAP, (uint64_t)dcbaa);
    w32(op, OP_CONFIG, (uint32_t)max_slots);

    if (ring_init(&cmd_ring) != 0) return -1;
    w64(op, OP_CRCR, (uint64_t)cmd_ring.trbs | 1);

    /* One event ring segment. */
    event_ring = dma_alloc(RING_TRBS * sizeof(trb_t), 4096);
    uint64_t *erst = dma_alloc(16, 64);
    if (!event_ring || !erst) return -1;
    erst[0] = (uint64_t)event_ring;
    erst[1] = RING_TRBS;
    w32(rt, IR0_ERSTSZ, 1);
    w64(rt, IR0_ERDP, (uint64_t)event_ring);
    w64(rt, IR0_ERSTBA, (uint64_t)erst);

    w32(op, OP_USBCMD, CMD_RUN);
    for (int i = 0; i < 100 && (r32(op, OP_USBSTS) & STS_HALTED); i++) delay_ms(1);
    if (r32(op, OP_USBSTS) & STS_HALTED) { kprintf("xHCI: controller did not start\n"); return -1; }

    /* Power the ports (most are powered already) and give devices time to connect. */
    for (int p = 1; p <= max_ports; p++) {
        uint32_t off = OP_PORTSC + 0x10 * (uint32_t)(p - 1);
        uint32_t v = r32(op, off);
        if (!(v & PORT_POWER)) w32(op, off, (v & PORT_KEEP) | PORT_POWER);
    }
    delay_ms(100);
    return 0;
}

static void describe(void) {
    char extra[48] = "";
    if (keyboard_count || mouse_count)
        k_snprintf(extra, sizeof(extra), ": %d keyboard%s, %d %s", keyboard_count,
                   keyboard_count == 1 ? "" : "s", mouse_count, mouse_count == 1 ? "mouse" : "mice");
    k_snprintf(description, sizeof(description), "%s xHCI, %d ports, %d device%s%s",
               pci_vendor_name(ctrl_vendor), max_ports, device_count, device_count == 1 ? "" : "s", extra);
}

int usb_init(void) {
    pci_device_t list[64];
    int n = pci_scan(list, 64);
    for (int i = 0; i < n; i++) {
        pci_device_t *p = &list[i];
        if (p->class_code != 0x0C || p->subclass != 0x03 || p->prog_if != 0x30) continue;
        uint32_t cmd = pci_config_read_dword(p->bus, p->slot, p->func, 0x04);
        pci_config_write_dword(p->bus, p->slot, p->func, 0x04, cmd | 0x2 | 0x4);   /* memory, bus master */
        uint32_t bar0 = pci_config_read_dword(p->bus, p->slot, p->func, 0x10);
        uint64_t base = bar0 & ~0xFu;
        if (((bar0 >> 1) & 3) == 2) base |= (uint64_t)pci_config_read_dword(p->bus, p->slot, p->func, 0x14) << 32;
        if (!base) continue;
        ctrl_vendor = p->vendor;
        ctrl_device = p->device;
        busy = 1;
        if (controller_start(base) != 0) {
            busy = 0;
            k_snprintf(description, sizeof(description), "%s xHCI did not start", pci_vendor_name(p->vendor));
            return -1;
        }
        found = 1;
        scan_ports();
        busy = 0;
        timer_add_poll_hook(usb_tick);
        describe();
        return mouse_count;
    }
    k_snprintf(description, sizeof(description), "no USB 3 (xHCI) controller");
    return -1;
}

const char *usb_description(void) { return description; }

/* A device was plugged in or out: set up the new ones (from the shell's
 * idle loop, not the timer interrupt, because this waits for answers). */
void usb_service(void) {
    if (!found || !rescan_wanted) return;
    rescan_wanted = 0;
    busy = 1;
    /* Forget devices whose port is now empty (their slot stays used). */
    for (int i = 0; i < device_count; i++) {
        uint32_t v = r32(op, OP_PORTSC + 0x10 * (uint32_t)(devices[i].port - 1));
        w32(op, OP_PORTSC + 0x10 * (uint32_t)(devices[i].port - 1), (v & PORT_KEEP) | (v & PORT_CHANGES));
        if (!(v & PORT_CONNECT)) {
            for (int j = 0; j < devices[i].hid_count; j++) {
                if (devices[i].hid[j].kind == HID_MOUSE) mouse_count--;
                else keyboard_count--;
            }
            devices[i] = devices[--device_count];
            i--;
        }
    }
    for (int p = 1; p <= max_ports; p++) {                  /* clear connect changes */
        uint32_t off = OP_PORTSC + 0x10 * (uint32_t)(p - 1);
        uint32_t v = r32(op, off);
        if (v & PORT_CHANGES) w32(op, off, (v & PORT_KEEP) | (v & PORT_CHANGES));
    }
    scan_ports();
    busy = 0;
    describe();
}

static const char *speed_name(int s) {
    switch (s) {
    case SPEED_LOW:   return "1.5 Mbit/s";
    case SPEED_FULL:  return "12 Mbit/s";
    case SPEED_HIGH:  return "480 Mbit/s";
    case SPEED_SUPER: return "5 Gbit/s";
    default:          return "?";
    }
}

void usb_print_devices(void) {
    usb_service();
    if (!found) {
        kprintf("%s\n", description[0] ? description : "USB is not set up");
        return;
    }
    kprintf("%s (%04x:%04x)\n", description, ctrl_vendor, ctrl_device);
    if (!device_count) kprintf("  nothing plugged in (plug a USB mouse in, then run lsusb again)\n");
    for (int i = 0; i < device_count; i++) {
        usb_device_t *d = &devices[i];
        int kinds = device_kinds(d);
        const char *what = kinds == (1 << HID_MOUSE | 1 << HID_KEYBOARD) ? "keyboard + mouse (in use)" :
                           kinds == 1 << HID_MOUSE ? "mouse (in use)" :
                           kinds == 1 << HID_KEYBOARD ? "keyboard (in use)" :
                           d->if_class == 3 ? "HID device" :
                           d->if_class == 8 ? "storage" : d->if_class == 9 || d->dev_class == 9 ? "hub" :
                           d->if_class == 0xE0 ? "wireless (Bluetooth?)" : d->if_class == 0x0E ? "camera" :
                           d->if_class == 1 ? "audio" : "device";
        kprintf("  port %2d  %04x:%04x  %-10s  %s%s%s\n", d->port, d->vendor, d->product, speed_name(d->speed),
                what, d->note ? " - " : "", d->note ? d->note : "");
    }
}
