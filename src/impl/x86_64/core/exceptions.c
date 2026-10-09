// CPU exception handling and the kernel panic screen.
//
// The panic screen deliberately does NOT use kprintf/print.c, the heap or the
// scrollback: the fault may well have been caused by one of them. It writes
// straight to the screen's cell grid (VGA memory, or the RAM grid that
// print_flush() draws on a framebuffer) and to COM1.

#include "core/exceptions.h"
#include "core/idt.h"
#include "core/gdt.h"
#include "lib/print.h"
#include "lib/serial.h"
#include "drivers/timer.h"
#include "sys/process.h"

extern void* isr_stub_table[32];             // exc_stubs.asm
extern char kernel_start[];
extern char stack_guard[];                   // unmapped page below the boot stack

#define VGA        (print_text_cells())
#define COLS       ((int)print_get_cols())   // 80x25, or larger on a framebuffer
#define ROWS       ((int)print_get_rows())
#define ATTR_PANIC 0x4F                      // white on red
#define ATTR_TITLE 0xF4                      // red on white

static const char* const exc_name[32] = {
    "Divide Error",           "Debug",                    "Non-Maskable Interrupt", "Breakpoint",
    "Overflow",               "Bound Range Exceeded",     "Invalid Opcode",         "Device Not Available",
    "Double Fault",           "Coprocessor Overrun",      "Invalid TSS",            "Segment Not Present",
    "Stack-Segment Fault",    "General Protection Fault", "Page Fault",             "Reserved",
    "x87 Floating-Point",     "Alignment Check",          "Machine Check",          "SIMD Floating-Point",
    "Virtualization",         "Control Protection",       "Reserved",               "Reserved",
    "Reserved",               "Reserved",                 "Reserved",               "Reserved",
    "Hypervisor Injection",   "VMM Communication",        "Security Exception",     "Reserved",
};

static const char* const exc_mnemonic[32] = {
    "#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM",
    "#DF", "---", "#TS", "#NP", "#SS", "#GP", "#PF", "---",
    "#MF", "#AC", "#MC", "#XM", "#VE", "#CP", "---", "---",
    "---", "---", "---", "---", "#HV", "#VC", "#SX", "---",
};

// ---- minimal output: VGA + serial -----------------------------------------

static int cur_row, cur_col;

static void out_char(char c) {
    serial_putc(c);
    if (c == '\n') { cur_col = 0; cur_row++; return; }
    if (cur_col >= COLS) { cur_col = 0; cur_row++; }
    if (cur_row < ROWS) VGA[cur_row * COLS + cur_col] = (uint16_t)((ATTR_PANIC << 8) | (uint8_t)c);
    cur_col++;
    print_flush();
}

static void out_str(const char* s) { while (*s) out_char(*s++); }

static void out_hex(uint64_t v, int digits) {
    for (int i = digits - 1; i >= 0; i--) out_char("0123456789ABCDEF"[(v >> (i * 4)) & 0xF]);
}

static void out_dec(uint64_t v) {
    char tmp[21]; int n = 0;
    do { tmp[n++] = '0' + (v % 10); v /= 10; } while (v);
    while (n) out_char(tmp[--n]);
}

static void out_reg(const char* name, uint64_t v) {
    out_str(name); out_char('='); out_hex(v, 16); out_char(' ');
}

static void screen_begin(void) {
    print_hide_cursor();
    for (int i = 0; i < COLS * ROWS; i++) VGA[i] = (uint16_t)((ATTR_PANIC << 8) | ' ');
    cur_row = 0; cur_col = 0;
    // Title bar
    const char* title = " KERNEL PANIC ";
    serial_puts("\n\n*** KERNEL PANIC ***\n");
    int start = (COLS - 14) / 2;
    for (int i = 0; i < COLS; i++) VGA[i] = (uint16_t)((ATTR_TITLE << 8) | ' ');
    for (int i = 0; title[i]; i++) VGA[start + i] = (uint16_t)((ATTR_TITLE << 8) | (uint8_t)title[i]);
    cur_row = 2;
    print_flush();
}

static uint64_t read_cr(int n) {
    uint64_t v = 0;
    switch (n) {
        case 0: __asm__ volatile("mov %%cr0, %0" : "=r"(v)); break;
        case 2: __asm__ volatile("mov %%cr2, %0" : "=r"(v)); break;
        case 3: __asm__ volatile("mov %%cr3, %0" : "=r"(v)); break;
        case 4: __asm__ volatile("mov %%cr4, %0" : "=r"(v)); break;
    }
    return v;
}

static int in_guard_page(uint64_t addr) {
    return addr >= (uint64_t)stack_guard && addr < (uint64_t)stack_guard + 4096;
}

// The identity-mapped kernel area; only dereference stack words inside it so
// that the panic code itself can never fault.
static int stack_readable(uint64_t addr) {
    return addr >= (uint64_t)kernel_start && addr + 8 <= 0x400000 && (addr & 7) == 0
        && !in_guard_page(addr);
}

static void out_footer(uint64_t rip) {
    out_str("\nSystem halted. Reboot the machine to continue.\n");
    if (rip) {
        out_str("Locate the faulting code (DEBUG=1 build):\n  x86_64-elf-addr2line -e dist/x86_64/kernel.bin 0x");
        out_hex(rip, 16);
        out_char('\n');
    }
    for (;;) __asm__ volatile("cli; hlt");
}

static volatile int panicking = 0;

static void begin_or_halt(void) {
    if (panicking) for (;;) __asm__ volatile("cli; hlt");   // fault inside the panic code
    panicking = 1;
    __asm__ volatile("cli");
    screen_begin();
}

// ---- decoding helpers -----------------------------------------------------

static void describe_page_fault(uint64_t err, uint64_t cr2) {
    out_str("cause: ");
    out_str((err & 1) ? "protection violation" : "page not present");
    out_str((err & 2) ? ", write access" : ", read access");
    out_str((err & 4) ? ", user mode" : ", kernel mode");
    if (err & 8)  out_str(", reserved bit set");
    if (err & 16) out_str(", instruction fetch");
    out_str("\nfaulting address (CR2) = 0x"); out_hex(cr2, 16); out_char('\n');
    if (in_guard_page(cr2))
        out_str("-> that is the guard page below the boot stack: KERNEL STACK OVERFLOW\n"
                "   (deep recursion or large local arrays; use kmalloc for big buffers)\n");
    else if (cr2 < 4096)
        out_str("-> looks like a NULL pointer dereference\n");
    else
        out_str("-> address is not mapped (only kernel, heap, page tables and VGA are)\n");
}

static void describe_gp(uint64_t err) {
    out_str("cause: ");
    if (err & 2) {
        out_str("interrupt vector 0x"); out_hex(err >> 3, 2);
        out_str(" fired but has no IDT handler installed\n");
    } else if (err) {
        out_str("bad segment selector 0x"); out_hex(err, 4); out_char('\n');
    } else {
        out_str("e.g. non-canonical address, privileged instruction, misaligned SSE access\n");
    }
}

// ---- public entry points --------------------------------------------------

void exception_handler(struct exc_frame* f) {
    // Debug and breakpoint traps are not errors: report and resume.
    if (f->vector == 1 || f->vector == 3) {
        kprintf("[EXC] %s at rip=0x%lx\n", exc_name[f->vector], f->rip);
        return;
    }

    // A user program faulted: end the program, not the kernel.
    if ((f->cs & 3) == 3) {
        uint32_t uv = f->vector < 32 ? (uint32_t)f->vector : 31;
        process_fault(f->vector, exc_name[uv], f->rip, f->vector == 14 ? read_cr(2) : 0);
    }

    begin_or_halt();

    uint32_t v = (uint32_t)f->vector;
    if (v >= 32) v = 31;
    out_str("CPU EXCEPTION  "); out_str(exc_mnemonic[v]); out_str("  ");
    out_str(exc_name[v]);
    out_str("   (vector "); out_dec(f->vector); out_str(")\n");
    out_str("error code = 0x"); out_hex(f->error, 16); out_char('\n');

    switch (f->vector) {
        case 14: describe_page_fault(f->error, read_cr(2)); break;
        case 13: describe_gp(f->error); break;
        case 8:
            out_str("cause: an exception occurred while delivering another one.\n");
            if (in_guard_page(f->rsp) || in_guard_page(f->rsp - 8) || f->rsp < (uint64_t)stack_guard)
                out_str("-> RSP is in/below the boot stack guard page: KERNEL STACK OVERFLOW\n");
            break;
        case 0:  out_str("cause: integer division by zero (or quotient overflow)\n"); break;
        case 6:  out_str("cause: CPU hit an invalid or unsupported instruction\n"); break;
        default: break;
    }

    out_str("\n");
    out_reg("RIP", f->rip); out_reg("CS", f->cs);     out_reg("RFLAGS", f->rflags); out_char('\n');
    out_reg("RSP", f->rsp); out_reg("SS", f->ss);     out_char('\n');
    out_str("\n");
    out_reg("RAX", f->rax); out_reg("RBX", f->rbx); out_reg("RCX", f->rcx); out_char('\n');
    out_reg("RDX", f->rdx); out_reg("RSI", f->rsi); out_reg("RDI", f->rdi); out_char('\n');
    out_reg("RBP", f->rbp); out_reg("R8 ", f->r8);  out_reg("R9 ", f->r9);  out_char('\n');
    out_reg("R10", f->r10); out_reg("R11", f->r11); out_reg("R12", f->r12); out_char('\n');
    out_reg("R13", f->r13); out_reg("R14", f->r14); out_reg("R15", f->r15); out_char('\n');
    out_reg("CR0", read_cr(0)); out_reg("CR3", read_cr(3)); out_reg("CR4", read_cr(4)); out_char('\n');

    out_str("\nstack @ RSP:");
    if (stack_readable(f->rsp) && stack_readable(f->rsp + 40)) {
        const uint64_t* sp = (const uint64_t*)f->rsp;
        for (int i = 0; i < 6; i++) { out_char(' '); out_hex(sp[i], 16); if (i == 2) out_str("\n             "); }
        out_char('\n');
    } else {
        out_str(" (RSP not in readable kernel memory)\n");
    }
    out_str("uptime ticks: "); out_dec(get_tick()); out_char('\n');

    out_footer(f->rip);
}

void kpanic(const char* message) {
    uint64_t caller = (uint64_t)__builtin_return_address(0);
    begin_or_halt();
    out_str("kpanic: "); out_str(message); out_str("\n\n");
    out_reg("caller", caller); out_char('\n');
    out_reg("CR0", read_cr(0)); out_reg("CR3", read_cr(3)); out_reg("CR4", read_cr(4)); out_char('\n');
    out_str("uptime ticks: "); out_dec(get_tick()); out_char('\n');
    out_footer(caller);
    __builtin_unreachable();
}

void exceptions_init(void) {
    for (int v = 0; v < 32; v++) {
        // NMI, #DF and #MC run on the dedicated IST stack (see gdt.c).
        uint8_t ist = (v == 2 || v == 8 || v == 18) ? IST_FATAL : 0;
        idt_set_entry_ist(v, isr_stub_table[v], 0x8E, ist);
    }
}
