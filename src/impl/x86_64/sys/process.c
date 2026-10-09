// process.c - load ELF programs and run them in user mode (ring 3)
//
// One program runs at a time, in the foreground: the shell calls
// process_run(), which maps the program into the user range (1-2 GiB), drops
// to ring 3 with user_enter() and gets control back when the program exits,
// faults or is interrupted with Ctrl+C. The kernel stays mapped (identity, no
// user bit), so system calls can read user memory directly once the pointer
// has been checked with user_range_ok().
#include "sys/process.h"
#include "sys/syscall_nums.h"
#include "core/gdt.h"
#include "core/exceptions.h"
#include "drivers/paging.h"
#include "drivers/memory.h"
#include "drivers/heap.h"
#include "drivers/fat32.h"
#include "drivers/keyboard.h"
#include "drivers/timer.h"
#include "drivers/rtc.h"
#include "lib/print.h"
#include "lib/string.h"

extern int64_t user_enter(uint64_t entry, uint64_t user_rsp);   // usermode.asm
extern void user_return(int64_t code) __attribute__((noreturn));

#define USER_STACK_SIZE  (256 * 1024)
#define USER_HEAP_MAX    (256ULL * 1024 * 1024)
#define MAX_ARGS         16
#define MAX_FDS          8
#define MAX_PATH         64
#define PAGE_UP(x)       (((x) + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1))
#define PAGE_DOWN(x)     ((x) & ~(uint64_t)(PAGE_SIZE - 1))

_Static_assert(KEYCODE_UP == KEY_UP_ARROW && KEYCODE_DOWN == KEY_DOWN_ARROW &&
               KEYCODE_LEFT == KEY_LEFT_ARROW && KEYCODE_RIGHT == KEY_RIGHT_ARROW,
               "user key codes must match the keyboard driver");

/* Interrupts and system calls from ring 3 arrive on this stack (TSS.rsp0). */
static uint8_t kernel_stack[32 * 1024] __attribute__((aligned(16)));

static struct {
    int running;
    char name[16];
    uint64_t image_lo, image_hi;      // pages holding the ELF segments
    uint64_t brk_start, brk, brk_mapped;
    uint64_t stack_lo;
} proc;

typedef struct {
    int used, mode;                   // OPEN_READ / OPEN_WRITE / OPEN_APPEND
    char path[MAX_PATH];
    uint8_t *data;
    uint32_t size, capacity, pos;
} file_t;
static file_t files[MAX_FDS];

/* Keyboard input for read(0): one line at a time, like a terminal. */
static char line_buf[256];
static uint32_t line_len, line_pos;

/* ---- ELF ---------------------------------------------------------------- */

typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed)) elf64_phdr_t;

#define ELF_TYPE_EXEC   2
#define ELF_MACHINE_X64 62
#define PT_LOAD         1

/* ---- User memory ------------------------------------------------------- */

static int map_user_range(uint64_t lo, uint64_t hi) {
    for (uint64_t v = PAGE_DOWN(lo); v < hi; v += PAGE_SIZE) {
        if (paging_get_entry(v) & PAGE_PRESENT) continue;
        void *frame = alloc_frame();
        if (!frame) return -1;
        memset(frame, 0, PAGE_SIZE);
        map_page(v, (uint64_t)frame, PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }
    return 0;
}

static void unmap_user_range(uint64_t lo, uint64_t hi) {
    for (uint64_t v = PAGE_DOWN(lo); v < hi; v += PAGE_SIZE) {
        uint64_t phys = unmap_page(v);
        if (phys) free_frame((void *)phys);
    }
}

/* 1 if the program may read (or, with `write`, write) [ptr, ptr + len). */
static int user_range_ok(uint64_t ptr, uint64_t len, int write) {
    if (len == 0) return 1;
    if (ptr < USER_BASE || ptr + len > USER_TOP || ptr + len < ptr) return 0;
    for (uint64_t v = PAGE_DOWN(ptr); v < ptr + len; v += PAGE_SIZE) {
        uint64_t e = paging_get_entry(v);
        if (!(e & PAGE_PRESENT) || !(e & PAGE_USER)) return 0;
        if (write && !(e & PAGE_RW)) return 0;
    }
    return 1;
}

/* Copy a NUL-terminated string from the program; 0 on success. */
static int copy_user_string(char *out, uint64_t ptr, size_t max) {
    for (size_t i = 0; i < max; i++) {
        if (!user_range_ok(ptr + i, 1, 0)) return -1;
        out[i] = *(const char *)(ptr + i);
        if (!out[i]) return 0;
    }
    return -1;
}

/* The user range must not overlap kernel mappings (heap, RAM disk, framebuffer). */
static int user_window_free(void) {
    static int checked = 0, ok = 0;
    if (!checked) {
        checked = 1;
        ok = 1;
        for (uint64_t v = USER_BASE; v < USER_TOP; v += PAGE_SIZE) {
            uint64_t e = paging_get_entry(v);
            if ((e & PAGE_PRESENT) && !(e & PAGE_USER)) { ok = 0; break; }
        }
    }
    return ok;
}

/* ---- Ending a program -------------------------------------------------- */

static void __attribute__((noreturn)) end_program(int64_t code) {
    user_return(code);
}

void process_fault(uint64_t vector, const char *name, uint64_t rip, uint64_t address) {
    if (!proc.running) return;
    print_set_theme_colors();
    if (print_get_col() != 0) print_str("\n");
    char msg[160];
    if (vector == 14)
        k_snprintf(msg, sizeof(msg), "%s: %s at address 0x%lx (rip 0x%lx) - program terminated",
                   proc.name, address < USER_BASE || address >= USER_TOP ? "Segmentation fault (kernel or unmapped memory)" : "Segmentation fault",
                   address, rip);
    else
        k_snprintf(msg, sizeof(msg), "%s: %s (rip 0x%lx) - program terminated", proc.name, name, rip);
    print_error(msg);
    int64_t code = vector == 0 ? EXIT_ARITHMETIC : vector == 6 ? EXIT_ILLEGAL : EXIT_SEGFAULT;
    end_program(code);
}

static void __attribute__((noreturn)) interrupted(void) {
    keyboard_ctrl_c = 0;
    print_set_theme_colors();
    print_str("^C\n");
    end_program(EXIT_INTERRUPTED);
}

/* Timer interrupt that arrived in user mode (irq.asm). */
void user_check_interrupt(void) {
    if (proc.running && keyboard_ctrl_c) interrupted();
}

/* ---- Files ------------------------------------------------------------- */

static int file_grow(file_t *f, uint32_t need) {
    if (need <= f->capacity) return 0;
    uint32_t cap = f->capacity ? f->capacity : 4096;
    while (cap < need) cap *= 2;
    uint8_t *data = kmalloc(cap);
    if (!data) return -1;
    if (f->size) memcpy(data, f->data, f->size);
    kfree(f->data);
    f->data = data;
    f->capacity = cap;
    return 0;
}

static int64_t sys_open(uint64_t path_ptr, uint64_t mode) {
    char path[MAX_PATH];
    if (copy_user_string(path, path_ptr, sizeof(path)) != 0) return SYSERR_FAULT;
    if (mode > OPEN_APPEND) return SYSERR_BADCALL;
    int fd = -1;
    for (int i = 3; i < MAX_FDS; i++)
        if (!files[i].used) { fd = i; break; }
    if (fd < 0) return SYSERR_MFILE;

    file_t *f = &files[fd];
    memset(f, 0, sizeof(*f));
    f->mode = (int)mode;
    memcpy(f->path, path, sizeof(path));

    int exists = fat32_file_exists(path);
    if (mode == OPEN_READ || (mode == OPEN_APPEND && exists)) {
        uint32_t size = fat32_get_file_size(path);
        if (!exists || size == 0xFFFFFFFF) return SYSERR_NOENT;
        if (file_grow(f, size + 1) != 0) return SYSERR_NOMEM;
        if (size && fat32_read_file(path, f->data, size) < 0) {
            kfree(f->data);
            return SYSERR_IO;
        }
        f->size = size;
    }
    f->used = 1;
    return fd;
}

static int64_t sys_close(uint64_t fd) {
    if (fd < 3 || fd >= MAX_FDS || !files[fd].used) return SYSERR_BADFD;
    file_t *f = &files[fd];
    int64_t result = 0;
    if (f->mode != OPEN_READ) {
        fat32_create_file(f->path);                 // fails harmlessly if it exists
        if (fat32_write_file(f->path, f->data ? f->data : (const uint8_t *)"", f->size) < 0)
            result = SYSERR_IO;
    }
    kfree(f->data);
    memset(f, 0, sizeof(*f));
    return result;
}

static void close_all_files(void) {
    for (int i = 3; i < MAX_FDS; i++)
        if (files[i].used) sys_close(i);
}

/* ---- Keyboard ---------------------------------------------------------- */

static void read_keyboard_line(void) {
    line_len = line_pos = 0;
    while (1) {
        int c = get_char();
        if (!c) {
            keyboard_idle();
            continue;
        }
        if (c == KEY_CTRL_C) interrupted();
        if (c == '\n') {
            print_str("\n");
            line_buf[line_len++] = '\n';
            return;
        }
        if (c == '\b') {
            if (line_len > 0) {
                line_len--;
                print_str("\b \b");
            }
            continue;
        }
        if (c >= 32 && c < 127 && line_len < sizeof(line_buf) - 1) {
            line_buf[line_len++] = (char)c;
            print_char((char)c);
        }
    }
}

/* ---- System calls ------------------------------------------------------ */

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    if (!user_range_ok(buf, len, 0)) return SYSERR_FAULT;
    const char *p = (const char *)buf;
    if (fd == 1 || fd == 2) {
        print_batch_begin();
        for (uint64_t i = 0; i < len; i++) print_char(p[i]);
        print_batch_end();
        return (int64_t)len;
    }
    if (fd >= MAX_FDS || !files[fd].used || files[fd].mode == OPEN_READ) return SYSERR_BADFD;
    file_t *f = &files[fd];
    if (len > 64 * 1024 * 1024 || file_grow(f, f->size + (uint32_t)len) != 0) return SYSERR_NOMEM;
    memcpy(f->data + f->size, p, len);
    f->size += (uint32_t)len;
    return (int64_t)len;
}

static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len) {
    if (!user_range_ok(buf, len, 1)) return SYSERR_FAULT;
    char *p = (char *)buf;
    if (fd == 0) {
        if (len == 0) return 0;
        if (line_pos >= line_len) read_keyboard_line();
        uint64_t n = 0;
        while (n < len && line_pos < line_len) p[n++] = line_buf[line_pos++];
        return (int64_t)n;
    }
    if (fd >= MAX_FDS || !files[fd].used || files[fd].mode != OPEN_READ) return SYSERR_BADFD;
    file_t *f = &files[fd];
    uint64_t n = f->size - f->pos;
    if (n > len) n = len;
    memcpy(p, f->data + f->pos, n);
    f->pos += (uint32_t)n;
    return (int64_t)n;
}

static int64_t sys_sbrk(int64_t increment) {
    uint64_t old = proc.brk, new_brk = proc.brk + (uint64_t)increment;
    if (new_brk < proc.brk_start || new_brk > proc.brk_start + USER_HEAP_MAX ||
        new_brk > proc.stack_lo - 16 * PAGE_SIZE)
        return SYSERR_NOMEM;
    if (PAGE_UP(new_brk) > proc.brk_mapped) {
        if (map_user_range(proc.brk_mapped, PAGE_UP(new_brk)) != 0) return SYSERR_NOMEM;
        proc.brk_mapped = PAGE_UP(new_brk);
    }
    proc.brk = new_brk;
    return (int64_t)old;
}

static int64_t sys_sleep(uint64_t ms) {
    uint32_t end = get_tick() + (uint32_t)((ms + 9) / 10);   // 100 Hz timer
    while ((int32_t)(end - get_tick()) > 0) {
        if (keyboard_ctrl_c) interrupted();
        __asm__ volatile("hlt");
    }
    return 0;
}

static int64_t sys_time(uint64_t ptr) {
    if (!user_range_ok(ptr, sizeof(struct os_time), 1)) return SYSERR_FAULT;
    rtc_time_t t;
    if (rtc_read(&t) != 0) return SYSERR_IO;
    rtc_add_minutes(&t, RTC_LOCAL_OFFSET_MIN);
    struct os_time *out = (struct os_time *)ptr;
    out->year = t.year; out->month = t.month; out->day = t.day;
    out->hour = t.hour; out->minute = t.minute; out->second = t.second;
    k_snprintf(out->zone, sizeof(out->zone), "%s", RTC_LOCAL_TZ_NAME);
    return 0;
}

static int64_t sys_getkey(void) {
    int c = get_char();
    if (c == KEY_CTRL_C) interrupted();
    return c;
}

static int64_t sys_console(uint64_t op, uint64_t a, uint64_t b) {
    switch (op) {
        case CON_CLEAR:  print_clear(); return 0;
        case CON_GOTO:   print_set_pos(a, b); return 0;
        case CON_COLOR:  print_set_color((uint8_t)(a & 15), (uint8_t)(b & 15)); return 0;
        case CON_RESET:  print_set_theme_colors(); return 0;
        case CON_SIZE:   return (int64_t)((print_get_cols() << 16) | print_get_rows());
        case CON_CURSOR: print_set_cursor_visible((int)a); return 0;
        default:         return SYSERR_BADCALL;
    }
}

static int64_t sys_readdir(uint64_t index, uint64_t ptr) {
    if (!user_range_ok(ptr, sizeof(struct os_dirent), 1)) return SYSERR_FAULT;
    fat32_file_info_t *list = kmalloc(32 * sizeof(fat32_file_info_t));
    if (!list) return SYSERR_NOMEM;
    int count = fat32_list_directory(list, 32);
    int64_t result = 0;
    if (count < 0) {
        result = SYSERR_IO;
    } else if (index < (uint64_t)count) {
        struct os_dirent *d = (struct os_dirent *)ptr;
        k_snprintf(d->name, sizeof(d->name), "%s", list[index].name);
        d->is_dir = list[index].is_directory;
        d->size = list[index].size;
        result = 1;
    }
    kfree(list);
    return result;
}

static int64_t sys_unlink(uint64_t path_ptr) {
    char path[MAX_PATH];
    if (copy_user_string(path, path_ptr, sizeof(path)) != 0) return SYSERR_FAULT;
    if (!fat32_file_exists(path)) return SYSERR_NOENT;
    return fat32_delete_file(path) == 0 ? 0 : SYSERR_IO;
}

/* int 0x80 (usermode.asm). Runs with interrupts on, so a blocking call
 * (keyboard, sleep) still gets timer and keyboard interrupts. */
void syscall_dispatch(struct exc_frame *f) {
    __asm__ volatile("sti");
    int64_t r;
    switch (f->rax) {
        case SYS_EXIT:    end_program((int64_t)(int32_t)f->rdi);
        case SYS_WRITE:   r = sys_write(f->rdi, f->rsi, f->rdx); break;
        case SYS_READ:    r = sys_read(f->rdi, f->rsi, f->rdx); break;
        case SYS_OPEN:    r = sys_open(f->rdi, f->rsi); break;
        case SYS_CLOSE:   r = sys_close(f->rdi); break;
        case SYS_SBRK:    r = sys_sbrk((int64_t)f->rdi); break;
        case SYS_SLEEP:   r = sys_sleep(f->rdi); break;
        case SYS_UPTIME:  r = (int64_t)get_tick() * 10; break;
        case SYS_TIME:    r = sys_time(f->rdi); break;
        case SYS_GETKEY:  r = sys_getkey(); break;
        case SYS_CONSOLE: r = sys_console(f->rdi, f->rsi, f->rdx); break;
        case SYS_READDIR: r = sys_readdir(f->rdi, f->rsi); break;
        case SYS_UNLINK:  r = sys_unlink(f->rdi); break;
        default:          r = SYSERR_BADCALL; break;
    }
    f->rax = (uint64_t)r;
}

/* ---- Loading and running ----------------------------------------------- */

const char *process_error_text(int err) {
    switch (err) {
        case PROC_ERR_NOT_FOUND: return "no such program";
        case PROC_ERR_NOT_ELF:   return "not an x86_64 ELF executable for this OS";
        case PROC_ERR_NO_MEMORY: return "out of memory";
        case PROC_ERR_BUSY:      return "a program is already running";
        case PROC_ERR_NO_WINDOW: return "kernel memory occupies the user address range";
        default:                 return "error";
    }
}

static void release_memory(void) {
    unmap_user_range(proc.image_lo, proc.image_hi);
    unmap_user_range(proc.brk_start, proc.brk_mapped);
    unmap_user_range(proc.stack_lo, USER_TOP);
}

/* Load the segments; fills proc.image_lo/hi. Returns the entry point or 0. */
static uint64_t load_elf(const uint8_t *data, uint32_t size) {
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)data;
    if (size < sizeof(*eh) || eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' ||
        eh->ident[3] != 'F' || eh->ident[4] != 2 /* 64-bit */ || eh->ident[5] != 1 /* little endian */ ||
        eh->type != ELF_TYPE_EXEC || eh->machine != ELF_MACHINE_X64 ||
        eh->phentsize != sizeof(elf64_phdr_t) || eh->phoff + (uint64_t)eh->phnum * sizeof(elf64_phdr_t) > size)
        return 0;

    uint64_t limit = USER_TOP - USER_STACK_SIZE - USER_HEAP_MAX;
    proc.image_lo = USER_TOP;
    proc.image_hi = USER_BASE;
    for (int i = 0; i < eh->phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(data + eh->phoff + i * sizeof(elf64_phdr_t));
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        if (ph->vaddr < USER_BASE || ph->vaddr + ph->memsz > limit || ph->filesz > ph->memsz ||
            ph->offset + ph->filesz > size)
            return 0;
        if (map_user_range(ph->vaddr, ph->vaddr + ph->memsz) != 0) return 0;
        if (PAGE_DOWN(ph->vaddr) < proc.image_lo) proc.image_lo = PAGE_DOWN(ph->vaddr);
        if (PAGE_UP(ph->vaddr + ph->memsz) > proc.image_hi) proc.image_hi = PAGE_UP(ph->vaddr + ph->memsz);
        memcpy((void *)ph->vaddr, data + ph->offset, ph->filesz);
    }
    if (proc.image_hi <= proc.image_lo) return 0;
    if (eh->entry < proc.image_lo || eh->entry >= proc.image_hi) return 0;
    return eh->entry;
}

/* argc, argv[], NULL and the strings at the top of the stack, as the SysV
 * ABI has it for _start. Returns the initial stack pointer. */
static uint64_t build_stack(int argc, char **argv) {
    uint64_t sp = USER_TOP;
    uint64_t ptrs[MAX_ARGS];
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        sp -= len;
        memcpy((void *)sp, argv[i], len);
        ptrs[i] = sp;
    }
    sp &= ~(uint64_t)15;
    // argc + argv[0..argc-1] + NULL: keep the final rsp 16-byte aligned.
    if ((argc + 2) % 2) sp -= 8;
    sp -= 8;
    *(uint64_t *)sp = 0;
    for (int i = argc - 1; i >= 0; i--) {
        sp -= 8;
        *(uint64_t *)sp = ptrs[i];
    }
    sp -= 8;
    *(uint64_t *)sp = (uint64_t)argc;
    return sp;
}

int process_run(const uint8_t *data, uint32_t size, int argc, char **argv) {
    if (proc.running) return PROC_ERR_BUSY;
    if (!user_window_free()) return PROC_ERR_NO_WINDOW;
    if (argc > MAX_ARGS) argc = MAX_ARGS;

    memset(&proc, 0, sizeof(proc));
    uint64_t entry = load_elf(data, size);
    if (!entry) {
        if (proc.image_hi > proc.image_lo) unmap_user_range(proc.image_lo, proc.image_hi);
        return PROC_ERR_NOT_ELF;
    }
    proc.stack_lo = USER_TOP - USER_STACK_SIZE;
    proc.brk_start = proc.brk = proc.brk_mapped = proc.image_hi;
    if (map_user_range(proc.stack_lo, USER_TOP) != 0) {
        release_memory();
        return PROC_ERR_NO_MEMORY;
    }
    uint64_t sp = build_stack(argc, argv);

    // Name for messages: the file name in upper case, without the extension.
    int n = 0;
    for (const char *p = argv[0]; *p && *p != '.' && n < (int)sizeof(proc.name) - 1; p++) proc.name[n++] = *p;
    proc.name[n] = 0;

    memset(files, 0, sizeof(files));
    line_len = line_pos = 0;
    keyboard_ctrl_c = 0;
    gdt_set_kernel_stack((uint64_t)(kernel_stack + sizeof(kernel_stack)));
    proc.running = 1;

    int64_t code = user_enter(entry, sp);

    __asm__ volatile("sti");
    proc.running = 0;
    close_all_files();
    release_memory();
    print_set_theme_colors();
    print_set_cursor_visible(1);
    if (print_get_col() != 0) print_str("\n");
    return (int)code;
}
