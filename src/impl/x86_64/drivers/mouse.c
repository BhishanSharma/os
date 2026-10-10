// mouse.c - PS/2 mouse, VirtualBox absolute pointer, console selection and paste
#include "drivers/mouse.h"
#include "drivers/keyboard.h"
#include "drivers/pic.h"
#include "drivers/vmmdev.h"
#include "drivers/touchpad.h"
#include "drivers/timer.h"
#include "sys/process.h"
#include "lib/print.h"
#include "lib/string.h"
#include "../lib/ports.h"

/* ---- 8042 controller and PS/2 mouse commands ------------------------------ */

#define PS2_DATA     0x60
#define PS2_STATUS   0x64           /* read */
#define PS2_COMMAND  0x64           /* write */
#define STATUS_OUT   0x01           /* a byte is waiting in PS2_DATA */
#define STATUS_IN    0x02           /* the controller has not taken our last byte yet */
#define STATUS_AUX   0x20           /* the waiting byte is from the mouse */

#define ACK          0xFA

static int wait_writable(void) {
    for (int i = 0; i < 100000; i++)
        if (!(inb(PS2_STATUS) & STATUS_IN)) return 0;
    return -1;
}

static int read_byte(uint8_t *out) {
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & STATUS_OUT) {
            *out = inb(PS2_DATA);
            return 0;
        }
    return -1;
}

static int controller_command(uint8_t cmd) {
    if (wait_writable() != 0) return -1;
    outb(PS2_COMMAND, cmd);
    return 0;
}

/* One byte to the mouse (through the controller); 0 when it acknowledges. */
static int mouse_command(uint8_t cmd) {
    if (controller_command(0xD4) != 0 || wait_writable() != 0) return -1;
    outb(PS2_DATA, cmd);
    for (int tries = 0; tries < 4; tries++) {
        uint8_t reply;
        if (read_byte(&reply) != 0) return -1;
        if (reply == ACK) return 0;
        if (reply == 0xFE || reply == 0xFC) return -1;   // resend / error
    }
    return -1;
}

static int set_sample_rate(uint8_t rate) {
    return mouse_command(0xF3) == 0 && mouse_command(rate) == 0 ? 0 : -1;
}

/* ---- State ----------------------------------------------------------------- */

typedef struct {
    int16_t dx, dy;
    int8_t wheel;
    uint8_t buttons;
} packet_t;

#define QUEUE_SIZE 64
static packet_t queue[QUEUE_SIZE];
static volatile uint32_t queue_head, queue_tail;

static int present, packet_size = 3, has_wheel;
static const char *other_source;      /* "USB", "I2C touchpad": a mouse that is not PS/2 */
static uint8_t packet[4];
static int packet_len;

/* Position in "mouse pixels": 8 per column and 16 per row of the text grid. */
static int px, py, seen;
static int buttons_held;
static int wheel_for_program;
static int owner_pid;

/* VirtualBox: the host's pointer position instead of PS/2 movement. */
#define VMMDEV_MOUSE_GUEST_CAN_ABSOLUTE    (1u << 0)
#define VMMDEV_MOUSE_HOST_WANTS_ABSOLUTE   (1u << 1)
#define VMMDEV_MOUSE_GUEST_NEEDS_HOST_CURSOR (1u << 2)

typedef struct {
    vmmdev_header_t header;
    uint32_t features;
    int32_t x, y;                    /* 0..0xFFFF across the screen */
} __attribute__((packed)) req_mouse_t;

static int vbox_absolute;
static req_mouse_t vbox_req;          /* static: below 4 GiB */

/* Console use: selection and clipboard. */
static int selecting, select_c, select_r, select_moved;
static volatile int has_selection, copy_request, paste_request;

int mouse_selection_active(void) { return has_selection; }
void mouse_request_copy(void) { copy_request = 1; }
void mouse_request_paste(void) { paste_request = 1; }
static char clipboard[4096];

int mouse_present(void) { return present; }

const char *mouse_description(void) {
    if (!present) return "none";
    if (other_source && !packet_size) return other_source;
    if (vbox_absolute) return has_wheel ? "PS/2, wheel, VirtualBox integration" : "PS/2, VirtualBox integration";
    return has_wheel ? "PS/2, wheel" : "PS/2";
}

/* ---- Setup ----------------------------------------------------------------- */

int mouse_init(void) {
    while (inb(PS2_STATUS) & STATUS_OUT) inb(PS2_DATA);       // stale bytes
    if (controller_command(0xA8) != 0) return -1;             // enable the mouse port

    // Controller configuration: mouse interrupt (IRQ 12) on, mouse clock on.
    uint8_t config;
    if (controller_command(0x20) != 0 || read_byte(&config) != 0) return -1;
    config |= 0x02;
    config &= (uint8_t)~0x20;
    if (controller_command(0x60) != 0 || wait_writable() != 0) return -1;
    outb(PS2_DATA, config);

    if (mouse_command(0xF6) != 0) return -1;                  // defaults; no answer: no mouse

    // IntelliMouse handshake: sample rates 200, 100, 80 turn on the wheel,
    // and the mouse then reports ID 3 (or 4 with extra buttons).
    if (set_sample_rate(200) == 0 && set_sample_rate(100) == 0 && set_sample_rate(80) == 0 &&
        mouse_command(0xF2) == 0) {
        uint8_t id;
        if (read_byte(&id) == 0 && (id == 3 || id == 4)) {
            has_wheel = 1;
            packet_size = 4;
        }
    }
    set_sample_rate(100);
    if (mouse_command(0xF4) != 0) return -1;                  // start reporting
    while (inb(PS2_STATUS) & STATUS_OUT) inb(PS2_DATA);

    present = 1;
    enable_irq(12);
    return 0;
}

void mouse_init_vbox(void) {
    if (!present || !vmmdev_present()) return;
    memset(&vbox_req, 0, sizeof(vbox_req));
    vbox_req.features = VMMDEV_MOUSE_GUEST_CAN_ABSOLUTE | VMMDEV_MOUSE_GUEST_NEEDS_HOST_CURSOR;
    int rc = vmmdev_request(&vbox_req, VMMDEV_SET_MOUSE_STATUS, sizeof(vbox_req));
    vbox_absolute = rc >= 0;
    kprintf("VirtualBox mouse integration: rc=%d\n", rc);
}

/* ---- Interrupt side ------------------------------------------------------ */

void mouse_handle_byte(uint8_t byte) {
    if (packet_len == 0 && !(byte & 0x08)) return;            // bit 3 is always set in byte 0
    packet[packet_len++] = byte;
    if (packet_len < packet_size) return;
    packet_len = 0;

    uint8_t b0 = packet[0];
    packet_t p;
    p.buttons = b0 & 0x07;
    p.dx = (b0 & 0xC0) ? 0 : (int16_t)(packet[1] - ((b0 << 4) & 0x100));   // overflow: drop
    p.dy = (b0 & 0xC0) ? 0 : (int16_t)(packet[2] - ((b0 << 3) & 0x100));
    p.wheel = 0;
    if (packet_size == 4) {
        int8_t z = (int8_t)(packet[3] << 4) >> 4;              // low 4 bits, signed
        p.wheel = z;
    }
    uint32_t next = (queue_head + 1) % QUEUE_SIZE;
    if (next == queue_tail) return;                            // full: drop
    queue[queue_head] = p;
    queue_head = next;
}

void mouse_report(const char *source, int buttons, int dx, int dy, int wheel) {
    if (!present) {                                            // no PS/2 mouse: this one only
        present = 1;
        packet_size = 0;
    }
    other_source = source;
    packet_t p;
    p.buttons = (uint8_t)(buttons & 7);
    p.dx = (int16_t)dx;
    p.dy = (int16_t)-dy;                                       // queue holds PS/2 sense: up positive
    p.wheel = (int8_t)-wheel;                                  // and PS/2 wheel: negative = up
    uint32_t next = (queue_head + 1) % QUEUE_SIZE;
    if (next == queue_tail) return;
    queue[queue_head] = p;
    queue_head = next;
}

void mouse_irq(void) {
    for (int i = 0; i < 16; i++) {
        uint8_t status = inb(PS2_STATUS);
        if ((status & (STATUS_OUT | STATUS_AUX)) != (STATUS_OUT | STATUS_AUX)) break;
        mouse_handle_byte(inb(PS2_DATA));
    }
}

/* ---- Console behaviour --------------------------------------------------- */

static int grid_cols(void) { return (int)print_get_cols(); }
static int grid_rows(void) { return (int)print_grid_rows(); }

static void cell_of_pointer(int *c, int *r) {
    *c = px / 8;
    *r = py / 16;
}

static void paste(void) {
    size_t n = strlen(clipboard);
    while (n && clipboard[n - 1] == '\n') n--;               // no accidental Enter at the end
    for (size_t i = 0; i < n; i++)
        if (!keyboard_inject((unsigned char)clipboard[i])) break;
}

static int console_owns_mouse(void) {
    return !owner_pid || process_foreground() != owner_pid;
}

static void button_changes(uint8_t old, uint8_t now) {
    int c, r;
    cell_of_pointer(&c, &r);
    if (!console_owns_mouse()) return;

    if ((now & MOUSE_LEFT) && !(old & MOUSE_LEFT)) {           // press: start a selection
        selecting = 1;
        select_moved = 0;
        has_selection = 0;
        select_c = c;
        select_r = r;
        print_set_selection(-1, 0, 0, 0);
    }
    if (!(now & MOUSE_LEFT) && (old & MOUSE_LEFT) && selecting) {   // release: keep it shown
        selecting = 0;
        if (!select_moved) print_set_selection(-1, 0, 0, 0);   // a plain click
    }
    // Other buttons do nothing here: copy and paste are Ctrl+C and Ctrl+V.
}

/* Ctrl+C with text selected, Ctrl+V: asked for by the keyboard interrupt. */
static void clipboard_requests(void) {
    if (copy_request) {
        copy_request = 0;
        if (has_selection) print_selection_text(clipboard, sizeof(clipboard));
        has_selection = 0;
        print_set_selection(-1, 0, 0, 0);
    }
    if (paste_request) {
        paste_request = 0;
        paste();
    }
}

void mouse_poll(void) {
    touchpad_poll();                                           // I2C touchpads are polled
    clipboard_requests();
    if (!present) return;
    int cols = grid_cols(), rows = grid_rows();
    int moved = 0;

    while (queue_tail != queue_head) {
        packet_t p = queue[queue_tail];
        queue_tail = (queue_tail + 1) % QUEUE_SIZE;
        if (!vbox_absolute) {
            px += p.dx;
            py -= p.dy;                                        // PS/2: up is positive
            moved |= p.dx || p.dy;
        }
        seen = 1;
        if (p.wheel) {
            if (console_owns_mouse()) {
                if (p.wheel < 0) scroll_up_lines(3);
                else scroll_down_lines(3);
            } else {
                wheel_for_program += p.wheel;
            }
        }
        if (p.buttons != buttons_held) {
            // Apply the movement so far before the click.
            if (px < 0) px = 0;
            if (py < 0) py = 0;
            if (px > cols * 8 - 1) px = cols * 8 - 1;
            if (py > rows * 16 - 1) py = rows * 16 - 1;
            uint8_t old = (uint8_t)buttons_held;
            buttons_held = p.buttons;
            button_changes(old, p.buttons);
        }
    }

    if (vbox_absolute) {
        memset(&vbox_req, 0, sizeof(vbox_req));
        if (vmmdev_request(&vbox_req, VMMDEV_GET_MOUSE_STATUS, sizeof(vbox_req)) >= 0 &&
            (vbox_req.features & VMMDEV_MOUSE_HOST_WANTS_ABSOLUTE)) {
            int nx = (int)((int64_t)vbox_req.x * cols * 8 / 0x10000);
            int ny = (int)((int64_t)vbox_req.y * rows * 16 / 0x10000);
            if (nx != px || ny != py) {
                px = nx;
                py = ny;
                moved = 1;
                seen = 1;
            }
        }
    }

    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px > cols * 8 - 1) px = cols * 8 - 1;
    if (py > rows * 16 - 1) py = rows * 16 - 1;
    if (!seen || !print_pointer_supported()) return;

    int c, r;
    cell_of_pointer(&c, &r);
    if (moved && selecting && console_owns_mouse()) {
        if (c != select_c || r != select_r) select_moved = 1;
        if (select_moved) {
            print_set_selection(select_c, select_r, c, r);
            has_selection = 1;
        }
    }
    print_set_pointer(c, r);
}

/* ---- Programs ------------------------------------------------------------ */

void mouse_read(int *col, int *row, int *buttons, int *wheel) {
    int c, r;
    cell_of_pointer(&c, &r);
    int status_rows = (int)print_grid_rows() - (int)print_get_rows();
    *col = c;
    *row = r - status_rows;
    *buttons = buttons_held;
    *wheel = wheel_for_program;
    wheel_for_program = 0;
}

void mouse_set_owner(int pid) {
    owner_pid = pid;
    if (pid) {
        selecting = 0;
        print_set_selection(-1, 0, 0, 0);
    }
}

int mouse_owner(void) { return owner_pid; }
