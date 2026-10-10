// process.c - user programs: loading ELF files, system calls, ending programs
//
// Every program is a task (sys/task.c) with its own page tables: the user
// range (1-2 GiB) holds its code, heap and stack, and the kernel is mapped the
// same way in every address space, so a system call can read the program's
// memory directly once the pointer is checked (user_range_ok). Several
// programs can run at once; the timer switches between them. One of them may
// be in the foreground: it gets the keyboard and Ctrl+C.
#include "sys/process.h"
#include "sys/syscall_nums.h"
#include "sys/task.h"
#include "sys/users.h"
#include "sys/shell.h"
#include "sys/sysinfo.h"
#include "core/exceptions.h"
#include "drivers/paging.h"
#include "drivers/memory.h"
#include "drivers/heap.h"
#include "drivers/fat32.h"
#include "drivers/disk.h"
#include "drivers/keyboard.h"
#include "drivers/timer.h"
#include "drivers/rtc.h"
#include "lib/print.h"
#include "lib/string.h"

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

typedef struct {
    int used, mode;                   // OPEN_READ / OPEN_WRITE / OPEN_APPEND
    char path[MAX_PATH];
    uint8_t *data;
    uint32_t size, capacity, pos;
} file_t;

typedef struct {
    uint64_t brk_start, brk, brk_mapped;
    uint64_t stack_lo;
    file_t files[MAX_FDS];
    char line_buf[256];               // keyboard input for read(0), one line at a time
    uint32_t line_len, line_pos;
    char cwd[128];                    // its own current directory
    int ctrlc_mode;                   // CTRLC_END / CTRLC_KEY
} process_t;

static int foreground_pid;            // 0: the kernel (login, kernel shell) has the keyboard

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

/* ---- The current program -------------------------------------------- */

static process_t *P(void) {
    return (process_t *)task_current()->process;
}

/* 1 if Ctrl+C is a key for this task rather than the end of it. */
static int takes_ctrl_c(task_t *t) {
    return t->process && ((process_t *)t->process)->ctrlc_mode == CTRLC_KEY;
}

/* ---- User memory -------------------------------------------------------- */

static int map_user_range(uint64_t root, uint64_t lo, uint64_t hi) {
    for (uint64_t v = PAGE_DOWN(lo); v < hi; v += PAGE_SIZE) {
        if (paging_get_user_entry(root, v) & PAGE_PRESENT) continue;
        void *frame = alloc_frame();
        if (!frame) return -1;
        memset(frame, 0, PAGE_SIZE);
        if (paging_map_user(root, v, (uint64_t)frame) != 0) {
            free_frame(frame);
            return -1;
        }
    }
    return 0;
}

/* Copy into another address space through the pages' identity mapping. */
static void copy_to_space(uint64_t root, uint64_t vaddr, const void *src, uint64_t len) {
    const uint8_t *s = src;
    while (len) {
        uint64_t page_off = vaddr & (PAGE_SIZE - 1);
        uint64_t n = PAGE_SIZE - page_off;
        if (n > len) n = len;
        uint64_t phys = paging_get_user_entry(root, vaddr) & ~0xFFFULL;
        memcpy((void *)(phys + page_off), s, n);
        vaddr += n;
        s += n;
        len -= n;
    }
}

/* 1 if the running program may read (or, with `write`, write) [ptr, ptr + len). */
static int user_range_ok(uint64_t ptr, uint64_t len, int write) {
    if (len == 0) return 1;
    if (ptr < USER_BASE || ptr + len > USER_TOP || ptr + len < ptr) return 0;
    uint64_t root = task_current()->root;
    for (uint64_t v = PAGE_DOWN(ptr); v < ptr + len; v += PAGE_SIZE) {
        uint64_t e = paging_get_user_entry(root, v);
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

/* ---- Working directory ---------------------------------------------------
 * The FAT32 driver has one current directory (the shell's). File system calls
 * switch to the program's own for the duration of the call. */

static void enter_cwd(char *saved, uint32_t size) {
    saved[0] = 0;
    fat32_get_current_directory(saved, size);
    if (fat32_change_directory(P()->cwd) != 0) fat32_change_directory("/");
}

static void leave_cwd(const char *saved) {
    if (saved[0]) fat32_change_directory(saved);
}

/* ---- Files -------------------------------------------------------------- */

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
    char path[MAX_PATH], saved[256];
    if (copy_user_string(path, path_ptr, sizeof(path)) != 0) return SYSERR_FAULT;
    if (mode > OPEN_APPEND) return SYSERR_BADCALL;
    process_t *p = P();
    int fd = -1;
    for (int i = 3; i < MAX_FDS; i++)
        if (!p->files[i].used) { fd = i; break; }
    if (fd < 0) return SYSERR_MFILE;

    file_t *f = &p->files[fd];
    memset(f, 0, sizeof(*f));
    f->mode = (int)mode;
    memcpy(f->path, path, sizeof(path));

    int64_t result = fd;
    enter_cwd(saved, sizeof(saved));
    if (mode != OPEN_READ && !user_may_write(path)) {
        result = SYSERR_PERM;
    } else {
        int exists = fat32_file_exists(path);
        if (mode == OPEN_READ || (mode == OPEN_APPEND && exists)) {
            uint32_t size = exists ? fat32_get_file_size(path) : 0xFFFFFFFF;
            if (size == 0xFFFFFFFF) {
                result = SYSERR_NOENT;
            } else if (file_grow(f, size + 1) != 0) {
                result = SYSERR_NOMEM;
            } else {
                int r = size ? fat32_read_file(path, f->data, size) : 0;
                if (r < 0) result = r == FAT32_ERR_PERMISSION ? SYSERR_PERM : SYSERR_IO;
                f->size = size;
            }
        }
    }
    leave_cwd(saved);
    if (result < 0) {
        kfree(f->data);
        memset(f, 0, sizeof(*f));
    } else {
        f->used = 1;
    }
    return result;
}

static int64_t close_file(file_t *f) {
    int64_t result = 0;
    if (f->mode != OPEN_READ) {
        char saved[256];
        enter_cwd(saved, sizeof(saved));
        fat32_create_file(f->path);                 // fails harmlessly if it exists
        int r = fat32_write_file(f->path, f->data ? f->data : (const uint8_t *)"", f->size);
        if (r < 0) result = r == FAT32_ERR_PERMISSION ? SYSERR_PERM : SYSERR_IO;
        leave_cwd(saved);
    }
    kfree(f->data);
    memset(f, 0, sizeof(*f));
    return result;
}

static int64_t sys_close(uint64_t fd) {
    if (fd < 3 || fd >= MAX_FDS || !P()->files[fd].used) return SYSERR_BADFD;
    return close_file(&P()->files[fd]);
}

/* ---- Ending a program -------------------------------------------------- */

/* End the running program: save its files, free its memory, become a zombie
 * for the shell to collect. */
static void __attribute__((noreturn)) process_exit(int code) {
    task_t *t = task_current();
    process_t *p = t->process;
    if (p) {
        for (int i = 3; i < MAX_FDS; i++)
            if (p->files[i].used) close_file(&p->files[i]);
        kfree(p);
        t->process = 0;
    }
    uint64_t root = t->root;
    t->root = paging_kernel_root();
    paging_switch(t->root);
    paging_free_address_space(root);
    // Its programs live on; pid 1 (login) collects them when they end.
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *c = task_at(i);
        if (c->state != TASK_FREE && c->parent == t->pid) c->parent = 1;
    }
    if (foreground_pid == t->pid) {
        task_t *parent = task_by_pid(t->parent);
        foreground_pid = parent && parent->is_user && parent->state != TASK_ZOMBIE ? parent->pid : 0;
        print_set_theme_colors();
        print_set_cursor_visible(1);
    }
    task_exit(code);
}

/* Called where a program can be stopped safely: Ctrl+C, `kill`. */
static void check_killed(void) {
    task_t *t = task_current();
    if (t->is_user && t->kill_code) {
        if (t->kill_code == EXIT_INTERRUPTED && t->pid == foreground_pid) {
            print_set_theme_colors();
            print_str("^C\n");
        }
        process_exit(t->kill_code);
    }
}

void process_fault(uint64_t vector, const char *name, uint64_t rip, uint64_t address) {
    task_t *t = task_current();
    if (!t || !t->is_user) return;
    print_set_theme_colors();
    if (print_get_col() != 0) print_str("\n");
    char msg[200];
    if (vector == 14)
        k_snprintf(msg, sizeof(msg), "%s (pid %d): %s at address 0x%lx (rip 0x%lx) - program terminated",
                   t->name, t->pid,
                   address < USER_BASE || address >= USER_TOP ? "Segmentation fault (kernel or unmapped memory)"
                                                               : "Segmentation fault",
                   address, rip);
    else
        k_snprintf(msg, sizeof(msg), "%s (pid %d): %s (rip 0x%lx) - program terminated", t->name, t->pid, name, rip);
    print_error(msg);
    process_exit(vector == 0 ? EXIT_ARITHMETIC : vector == 6 ? EXIT_ILLEGAL : EXIT_SEGFAULT);
}

/* Timer interrupt that arrived in user mode (irq.asm): stop the program if it
 * was asked to, otherwise switch if its time slice is used up. */
void user_check_interrupt(void) {
    check_killed();
    task_preempt();
}

/* ---- Keyboard ----------------------------------------------------------- */

/* Wait until this program is in the foreground (background programs do not
 * get the keyboard). */
static void wait_for_foreground(void) {
    while (foreground_pid != task_current()->pid) {
        check_killed();
        task_sleep(50);
    }
}

static void read_keyboard_line(process_t *p) {
    p->line_len = p->line_pos = 0;
    while (1) {
        wait_for_foreground();
        check_killed();
        int c = get_char();
        if (!c) {
            task_sleep(10);
            continue;
        }
        if (c == KEY_CTRL_C) {
            if (p->ctrlc_mode == CTRLC_KEY) {      // drop the line, return an empty one
                keyboard_ctrl_c = 0;
                print_str("^C\n");
                p->line_buf[0] = '\n';
                p->line_len = 1;
                return;
            }
            task_current()->kill_code = EXIT_INTERRUPTED;
            check_killed();
        }
        if (c == '\n') {
            print_str("\n");
            p->line_buf[p->line_len++] = '\n';
            return;
        }
        if (c == '\b') {
            if (p->line_len > 0) {
                p->line_len--;
                print_str("\b \b");
            }
            continue;
        }
        if (c >= 32 && c < 127 && p->line_len < sizeof(p->line_buf) - 1) {
            p->line_buf[p->line_len++] = (char)c;
            print_char((char)c);
        }
    }
}

/* ---- System calls ------------------------------------------------------ */

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    if (!user_range_ok(buf, len, 0)) return SYSERR_FAULT;
    const char *src = (const char *)buf;
    if (fd == 1 || fd == 2) {
        print_batch_begin();
        for (uint64_t i = 0; i < len; i++) print_char(src[i]);
        print_batch_end();
        return (int64_t)len;
    }
    process_t *p = P();
    if (fd >= MAX_FDS || !p->files[fd].used || p->files[fd].mode == OPEN_READ) return SYSERR_BADFD;
    file_t *f = &p->files[fd];
    if (len > 64 * 1024 * 1024 || file_grow(f, f->size + (uint32_t)len) != 0) return SYSERR_NOMEM;
    memcpy(f->data + f->size, src, len);
    f->size += (uint32_t)len;
    return (int64_t)len;
}

static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len) {
    if (!user_range_ok(buf, len, 1)) return SYSERR_FAULT;
    char *dst = (char *)buf;
    process_t *p = P();
    if (fd == 0) {
        if (len == 0) return 0;
        if (p->line_pos >= p->line_len) read_keyboard_line(p);
        uint64_t n = 0;
        while (n < len && p->line_pos < p->line_len) dst[n++] = p->line_buf[p->line_pos++];
        return (int64_t)n;
    }
    if (fd >= MAX_FDS || !p->files[fd].used || p->files[fd].mode != OPEN_READ) return SYSERR_BADFD;
    file_t *f = &p->files[fd];
    uint64_t n = f->size - f->pos;
    if (n > len) n = len;
    memcpy(dst, f->data + f->pos, n);
    f->pos += (uint32_t)n;
    return (int64_t)n;
}

static int64_t sys_sbrk(int64_t increment) {
    process_t *p = P();
    uint64_t old = p->brk, new_brk = p->brk + (uint64_t)increment;
    if (new_brk < p->brk_start || new_brk > p->brk_start + USER_HEAP_MAX ||
        new_brk > p->stack_lo - 16 * PAGE_SIZE)
        return SYSERR_NOMEM;
    if (PAGE_UP(new_brk) > p->brk_mapped) {
        if (map_user_range(task_current()->root, p->brk_mapped, PAGE_UP(new_brk)) != 0) return SYSERR_NOMEM;
        p->brk_mapped = PAGE_UP(new_brk);
    }
    p->brk = new_brk;
    return (int64_t)old;
}

static int64_t sys_sleep(uint64_t ms) {
    uint32_t end = get_tick() + (uint32_t)((ms + 9) / 10);   // 100 Hz timer
    while ((int32_t)(end - get_tick()) > 0) {
        check_killed();
        uint32_t left = (end - get_tick()) * 10;
        task_sleep(left > 50 ? 50 : left);
    }
    check_killed();
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

/* With `wait`, block until there is a key (and this program has the keyboard). */
static int64_t sys_getkey(uint64_t wait) {
    int c;
    while (1) {
        if (wait) wait_for_foreground();
        else if (foreground_pid != task_current()->pid) return 0;
        c = get_char();
        if (c || !wait) break;
        check_killed();
        task_sleep(10);
    }
    if (c == KEY_CTRL_C) {
        if (P()->ctrlc_mode == CTRLC_KEY) {
            keyboard_ctrl_c = 0;
            return c;
        }
        task_current()->kill_code = EXIT_INTERRUPTED;
        check_killed();
    }
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
        case CON_THEME:  print_use_theme_color((int)a); return 0;
        case CON_COLUMN: return (int64_t)print_get_col();
        default:         return SYSERR_BADCALL;
    }
}

static int64_t sys_readdir(uint64_t index, uint64_t ptr) {
    if (!user_range_ok(ptr, sizeof(struct os_dirent), 1)) return SYSERR_FAULT;
    fat32_file_info_t *list = kmalloc(32 * sizeof(fat32_file_info_t));
    if (!list) return SYSERR_NOMEM;
    char saved[256];
    enter_cwd(saved, sizeof(saved));
    int count = fat32_list_directory(list, 32);
    leave_cwd(saved);
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
    char path[MAX_PATH], saved[256];
    if (copy_user_string(path, path_ptr, sizeof(path)) != 0) return SYSERR_FAULT;
    enter_cwd(saved, sizeof(saved));
    int64_t result;
    if (!fat32_file_exists(path)) {
        result = SYSERR_NOENT;
    } else {
        int r = fat32_delete_file(path);
        result = r == 0 ? 0 : r == FAT32_ERR_PERMISSION ? SYSERR_PERM : SYSERR_IO;
    }
    leave_cwd(saved);
    return result;
}

static int64_t sys_getuser(uint64_t ptr) {
    if (!user_range_ok(ptr, sizeof(struct os_user), 1)) return SYSERR_FAULT;
    const user_t *u = user_current();
    struct os_user *out = (struct os_user *)ptr;
    out->uid = u->uid;
    k_snprintf(out->name, sizeof(out->name), "%s", u->name);
    k_snprintf(out->home, sizeof(out->home), "%s", u->home);
    return 0;
}

static int64_t sys_spawn(uint64_t path_ptr, uint64_t argv_ptr, uint64_t user_ptr);
static int64_t sys_wait(int64_t pid, uint64_t status_ptr, uint64_t flags);
static int64_t sys_kill(int64_t pid);
static int64_t sys_taskinfo(uint64_t slot, uint64_t ptr);
static int64_t sys_chdir(uint64_t path_ptr);
static int64_t sys_getcwd(uint64_t buf, uint64_t size);
static int64_t sys_kcommand(uint64_t line_ptr);
static int64_t sys_uname(uint64_t ptr);

/* int 0x80 (usermode.asm). Runs with interrupts on, so a blocking call
 * (keyboard, sleep) still gets timer and keyboard interrupts. */
void syscall_dispatch(struct exc_frame *f) {
    __asm__ volatile("sti");
    check_killed();
    int64_t r;
    switch (f->rax) {
        case SYS_EXIT:    process_exit((int)(int32_t)f->rdi);
        case SYS_WRITE:   r = sys_write(f->rdi, f->rsi, f->rdx); break;
        case SYS_READ:    r = sys_read(f->rdi, f->rsi, f->rdx); break;
        case SYS_OPEN:    r = sys_open(f->rdi, f->rsi); break;
        case SYS_CLOSE:   r = sys_close(f->rdi); break;
        case SYS_SBRK:    r = sys_sbrk((int64_t)f->rdi); break;
        case SYS_SLEEP:   r = sys_sleep(f->rdi); break;
        case SYS_UPTIME:  r = (int64_t)get_tick() * 10; break;
        case SYS_TIME:    r = sys_time(f->rdi); break;
        case SYS_GETKEY:  r = sys_getkey(f->rdi); break;
        case SYS_CONSOLE: r = sys_console(f->rdi, f->rsi, f->rdx); break;
        case SYS_READDIR: r = sys_readdir(f->rdi, f->rsi); break;
        case SYS_UNLINK:  r = sys_unlink(f->rdi); break;
        case SYS_GETUSER: r = sys_getuser(f->rdi); break;
        case SYS_GETPID:  r = task_current()->pid; break;
        case SYS_YIELD:   task_yield(); r = 0; break;
        case SYS_SPAWN:   r = sys_spawn(f->rdi, f->rsi, f->rdx); break;
        case SYS_WAIT:    r = sys_wait((int64_t)f->rdi, f->rsi, f->rdx); break;
        case SYS_KILL:    r = sys_kill((int64_t)f->rdi); break;
        case SYS_TASKINFO: r = sys_taskinfo(f->rdi, f->rsi); break;
        case SYS_CHDIR:   r = sys_chdir(f->rdi); break;
        case SYS_GETCWD:  r = sys_getcwd(f->rdi, f->rsi); break;
        case SYS_KCOMMAND: r = sys_kcommand(f->rdi); break;
        case SYS_CTRLC:   r = f->rdi <= CTRLC_KEY ? (P()->ctrlc_mode = (int)f->rdi, 0) : SYSERR_BADCALL; break;
        case SYS_UNAME:   r = sys_uname(f->rdi); break;
        default:          r = SYSERR_BADCALL; break;
    }
    f->rax = (uint64_t)r;
}

/* ---- Starting programs -------------------------------------------------- */

const char *process_error_text(int err) {
    switch (err) {
        case PROC_ERR_NOT_FOUND: return "no such program";
        case PROC_ERR_NOT_ELF:   return "not an x86_64 ELF executable for this OS";
        case PROC_ERR_NO_MEMORY: return "out of memory";
        case PROC_ERR_BUSY:      return "too many programs running";
        case PROC_ERR_NO_WINDOW: return "kernel memory occupies the user address range";
        default:                 return "error";
    }
}

/* Map and copy the segments into `root`. Returns the entry point (0 on
 * error) and the end of the image in *image_end. */
static uint64_t load_elf(uint64_t root, const uint8_t *data, uint32_t size, uint64_t *image_end) {
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)data;
    if (size < sizeof(*eh) || eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' ||
        eh->ident[3] != 'F' || eh->ident[4] != 2 /* 64-bit */ || eh->ident[5] != 1 /* little endian */ ||
        eh->type != ELF_TYPE_EXEC || eh->machine != ELF_MACHINE_X64 ||
        eh->phentsize != sizeof(elf64_phdr_t) || eh->phoff + (uint64_t)eh->phnum * sizeof(elf64_phdr_t) > size)
        return 0;

    uint64_t limit = USER_TOP - USER_STACK_SIZE - USER_HEAP_MAX;
    uint64_t lo = USER_TOP, hi = USER_BASE;
    for (int i = 0; i < eh->phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(data + eh->phoff + i * sizeof(elf64_phdr_t));
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        if (ph->vaddr < USER_BASE || ph->vaddr + ph->memsz > limit || ph->filesz > ph->memsz ||
            ph->offset + ph->filesz > size)
            return 0;
        if (map_user_range(root, ph->vaddr, ph->vaddr + ph->memsz) != 0) return 0;
        if (PAGE_DOWN(ph->vaddr) < lo) lo = PAGE_DOWN(ph->vaddr);
        if (PAGE_UP(ph->vaddr + ph->memsz) > hi) hi = PAGE_UP(ph->vaddr + ph->memsz);
        copy_to_space(root, ph->vaddr, data + ph->offset, ph->filesz);
    }
    if (hi <= lo || eh->entry < lo || eh->entry >= hi) return 0;
    *image_end = hi;
    return eh->entry;
}

/* argc, argv[], NULL and the strings at the top of the stack, as the SysV ABI
 * has it for _start. Built in a kernel copy of the top page, then copied in.
 * Returns the initial stack pointer, or 0 if the arguments do not fit. */
static uint64_t build_stack(uint64_t root, int argc, char **argv) {
    static uint8_t page[PAGE_SIZE];
    uint64_t base = USER_TOP - PAGE_SIZE;
    uint64_t sp = USER_TOP, ptrs[MAX_ARGS];
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        if (sp - len < base + 512) return 0;
        sp -= len;
        memcpy(page + (sp - base), argv[i], len);
        ptrs[i] = sp;
    }
    sp &= ~(uint64_t)15;
    if ((argc + 2) % 2) sp -= 8;          // keep the final rsp 16-byte aligned
    sp -= 8;
    *(uint64_t *)(page + (sp - base)) = 0;
    for (int i = argc - 1; i >= 0; i--) {
        sp -= 8;
        *(uint64_t *)(page + (sp - base)) = ptrs[i];
    }
    sp -= 8;
    *(uint64_t *)(page + (sp - base)) = (uint64_t)argc;
    copy_to_space(root, sp, page + (sp - base), USER_TOP - sp);
    return sp;
}

/* Start a program as `user`, in the FAT32 current directory. */
static int spawn_as(const uint8_t *data, uint32_t size, int argc, char **argv, const user_t *user) {
    if (!paging_user_range_free()) return PROC_ERR_NO_WINDOW;
    if (argc > MAX_ARGS) argc = MAX_ARGS;

    process_t *p = kmalloc(sizeof(process_t));
    uint64_t root = paging_create_address_space();
    if (!p || !root) {
        kfree(p);
        if (root) paging_free_address_space(root);
        return PROC_ERR_NO_MEMORY;
    }
    memset(p, 0, sizeof(*p));

    uint64_t image_end = 0, entry = load_elf(root, data, size, &image_end), sp = 0;
    int err = entry ? 0 : PROC_ERR_NOT_ELF;
    p->stack_lo = USER_TOP - USER_STACK_SIZE;
    if (!err && map_user_range(root, p->stack_lo, USER_TOP) != 0) err = PROC_ERR_NO_MEMORY;
    if (!err && !(sp = build_stack(root, argc, argv))) err = PROC_ERR_NO_MEMORY;
    if (err) {
        paging_free_address_space(root);
        kfree(p);
        return err;
    }
    p->brk_start = p->brk = p->brk_mapped = image_end;
    fat32_get_current_directory(p->cwd, sizeof(p->cwd));

    // Name for ps and messages: the file name without folders and extension.
    const char *base = argv[0];
    for (const char *s = argv[0]; *s; s++)
        if (*s == '/') base = s + 1;
    char name[16];
    int n = 0;
    for (const char *s = base; *s && *s != '.' && n < (int)sizeof(name) - 1; s++)
        name[n++] = (*s >= 'A' && *s <= 'Z') ? *s + 32 : *s;
    name[n] = 0;

    // The task starts running at the next switch, so set it up before it can.
    __asm__ volatile("cli");
    task_t *t = task_create_user(name, root, entry, sp);
    if (!t) {
        __asm__ volatile("sti");
        paging_free_address_space(root);
        kfree(p);
        return PROC_ERR_BUSY;
    }
    t->process = p;
    t->user = *user;
    __asm__ volatile("sti");
    return t->pid;
}

int process_spawn(const uint8_t *data, uint32_t size, int argc, char **argv) {
    return spawn_as(data, size, argc, argv, user_current());
}

int process_foreground(void) {
    return foreground_pid;
}

/* Wait until `t` ends, reap it and return its exit code. With `foreground`,
 * it has the keyboard meanwhile, and Ctrl+C ends it (unless it takes Ctrl+C
 * as a key, like the shell). */
static int wait_for(task_t *t, int foreground) {
    task_t *me = task_current();
    int pid = t->pid, previous = foreground_pid;
    if (foreground) {
        foreground_pid = pid;
        keyboard_ctrl_c = 0;
    }
    int interrupted = 0;
    while (t->state != TASK_ZOMBIE) {
        if (keyboard_ctrl_c && foreground_pid == pid && !takes_ctrl_c(t)) {
            keyboard_ctrl_c = 0;
            t->kill_code = EXIT_INTERRUPTED;
            interrupted = 1;
        }
        if (me->is_user && me->kill_code) {        // the waiter itself is being stopped
            if (foreground_pid == pid) foreground_pid = previous;
            check_killed();
        }
        if (me->pid == 1) process_reap_orphans();
        keyboard_idle();       // status bar, then sleep a tick
    }
    if (foreground && foreground_pid == pid) foreground_pid = previous;
    if (interrupted) keyboard_flush();   // the Ctrl+C has been dealt with
    int code = t->exit_code;
    task_reap(t);
    if (print_get_col() != 0) print_str("\n");
    return code;
}

int process_wait(int pid, int foreground) {
    task_t *t = task_by_pid(pid);
    if (!t || !t->is_user) return PROC_ERR_NOT_FOUND;
    return wait_for(t, foreground);
}

int process_kill(int pid, int code) {
    task_t *t = task_by_pid(pid);
    if (!t || !t->is_user || t->state == TASK_ZOMBIE) return PROC_ERR_NOT_FOUND;
    t->kill_code = code;
    return 0;
}

/* Programs whose parent ended belong to pid 1: free them once they end. */
void process_reap_orphans(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = task_at(i);
        if (t->state == TASK_ZOMBIE && t->is_user && t->parent == 1 && t != task_current()) task_reap(t);
    }
}

/* End of a login session: stop every program still running, and free them. */
void process_end_all(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = task_at(i);
        if (t->is_user && t->state != TASK_FREE && t->state != TASK_ZOMBIE) t->kill_code = EXIT_KILLED;
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = task_at(i);
        if (!t->is_user || t->state == TASK_FREE) continue;
        while (t->state != TASK_ZOMBIE) task_sleep(10);
        task_reap(t);
    }
    foreground_pid = 0;
}

/* ---- Finding programs --------------------------------------------------- */

/* Read `name` or `name.elf` from the current directory into a kmalloc'd buffer. */
static uint8_t *read_program_file(const char *name, uint32_t *size) {
    char path[MAX_PATH + 8];
    k_snprintf(path, sizeof(path), "%s", name);
    if (!fat32_file_exists(path)) {
        k_snprintf(path, sizeof(path), "%s.elf", name);
        if (!fat32_file_exists(path)) return 0;
    }
    uint32_t n = fat32_get_file_size(path);
    if (n == 0 || n == 0xFFFFFFFF) return 0;
    uint8_t *data = kmalloc(n);
    if (!data) return 0;
    if (fat32_read_file(path, data, n) < 0) {
        kfree(data);
        return 0;
    }
    *size = n;
    return data;
}

uint8_t *process_find_program(const char *name, uint32_t *size) {
    uint8_t *data = read_program_file(name, size);
    int has_folder = 0;
    for (const char *c = name; *c; c++)
        if (*c == '/') has_folder = 1;
    if (data || !disk_ramdisk_size() || has_folder) return data;

    char cwd[256] = "/";
    fat32_get_current_directory(cwd, sizeof(cwd));
    disk_kind_t old = disk_selected();
    if (old != DISK_RAM) {
        disk_select(DISK_RAM);
        if (fat32_init(0) != 0) {
            disk_select(old);
            if (old != DISK_NONE) fat32_init(0);
            return 0;
        }
    }
    fat32_change_directory("/");
    data = read_program_file(name, size);
    if (old != DISK_RAM) {
        disk_select(old);
        if (old != DISK_NONE) fat32_init(0);
    }
    fat32_change_directory(cwd);
    return data;
}

/* ---- System calls for programs that run programs ----------------------- */

static int64_t proc_error(int err) {
    switch (err) {
        case PROC_ERR_NOT_FOUND: return SYSERR_NOENT;
        case PROC_ERR_NOT_ELF:   return SYSERR_NOEXEC;
        case PROC_ERR_BUSY:      return SYSERR_AGAIN;
        default:                 return SYSERR_NOMEM;
    }
}

/* spawn(path, argv, as_user) */
static int64_t sys_spawn(uint64_t path_ptr, uint64_t argv_ptr, uint64_t user_ptr) {
    char path[MAX_PATH], saved[256];
    if (copy_user_string(path, path_ptr, sizeof(path)) != 0) return SYSERR_FAULT;

    // The arguments: copied into one kernel buffer before anything can change.
    char *strings = kmalloc(1024), *argv[MAX_ARGS];
    if (!strings) return SYSERR_NOMEM;
    int argc = 0;
    size_t used = 0;
    if (argv_ptr) {
        for (; argc < MAX_ARGS; argc++) {
            uint64_t slot = argv_ptr + (uint64_t)argc * 8;
            if (!user_range_ok(slot, 8, 0)) { kfree(strings); return SYSERR_FAULT; }
            uint64_t s = *(const uint64_t *)slot;
            if (!s) break;
            if (used >= 1024 || copy_user_string(strings + used, s, 1024 - used) != 0) {
                kfree(strings);
                return SYSERR_FAULT;
            }
            argv[argc] = strings + used;
            used += strlen(argv[argc]) + 1;
        }
    }
    if (argc == 0) argv[argc++] = path;

    user_t as = *user_current();
    if (user_ptr) {
        char name[32];
        if (copy_user_string(name, user_ptr, sizeof(name)) != 0) {
            kfree(strings);
            return SYSERR_FAULT;
        }
        int r = users_authenticate(name, &as);
        if (r != 0) {
            kfree(strings);
            return r == USERS_NO_SUCH_USER ? SYSERR_NOUSER : SYSERR_AUTH;
        }
    }

    enter_cwd(saved, sizeof(saved));
    uint32_t size = 0;
    // As another user (su): only system programs, never one from this folder.
    if (user_ptr) fat32_change_directory("/");
    uint8_t *image = process_find_program(path, &size);
    if (user_ptr && fat32_change_directory(P()->cwd) != 0) fat32_change_directory("/");
    int64_t result = image ? spawn_as(image, size, argc, argv, &as) : PROC_ERR_NOT_FOUND;
    leave_cwd(saved);
    kfree(image);
    kfree(strings);
    return result < 0 ? proc_error((int)result) : result;
}

/* wait(pid, &status, flags): a child of the caller; pid -1 = any of them. */
static int64_t sys_wait(int64_t pid, uint64_t status_ptr, uint64_t flags) {
    if (status_ptr && !user_range_ok(status_ptr, sizeof(int), 1)) return SYSERR_FAULT;
    if (pid == -1 && (flags & WAIT_FOREGROUND)) return SYSERR_BADCALL;
    task_t *me = task_current();
    while (1) {
        task_t *child = 0, *ended = 0;
        for (int i = 0; i < MAX_TASKS; i++) {
            task_t *t = task_at(i);
            if (t->state == TASK_FREE || !t->is_user || t->parent != me->pid) continue;
            if (pid != -1 && t->pid != pid) continue;
            child = t;
            if (t->state == TASK_ZOMBIE) { ended = t; break; }
        }
        if (!child) return SYSERR_CHILD;
        if (pid != -1 && !(flags & WAIT_NOHANG)) ended = child;   // wait for this one
        if (ended) {
            int ended_pid = ended->pid;
            int code = wait_for(ended, (flags & WAIT_FOREGROUND) != 0);
            if (status_ptr) *(int *)status_ptr = code;
            return ended_pid;
        }
        if (flags & WAIT_NOHANG) return 0;
        check_killed();
        task_sleep(20);
    }
}

static int64_t sys_kill(int64_t pid) {
    task_t *t = task_by_pid((int)pid);
    if (!t || t->state == TASK_ZOMBIE) return SYSERR_SRCH;
    if (!t->is_user) return SYSERR_PERM;                // part of the kernel
    if (!user_is_root() && t->user.uid != user_current()->uid) return SYSERR_PERM;
    t->kill_code = EXIT_KILLED;
    return 0;
}

static int64_t sys_taskinfo(uint64_t slot, uint64_t ptr) {
    if (slot >= MAX_TASKS) return SYSERR_SRCH;
    if (!user_range_ok(ptr, sizeof(struct os_task), 1)) return SYSERR_FAULT;
    task_t *t = task_at((int)slot);
    if (t->state == TASK_FREE) return 0;
    struct os_task *out = (struct os_task *)ptr;
    memset(out, 0, sizeof(*out));
    out->pid = t->pid;
    out->parent = t->parent;
    out->state = t->state == TASK_ZOMBIE ? TASKSTATE_DONE
               : t->state == TASK_SLEEPING ? TASKSTATE_SLEEPING
               : t == task_current() ? TASKSTATE_RUNNING : TASKSTATE_READY;
    out->is_program = t->is_user;
    out->foreground = t->pid == foreground_pid || (!foreground_pid && t->pid == 1);
    out->cpu_ms = t->ticks * 10;
    k_snprintf(out->name, sizeof(out->name), "%s", t->name);
    k_snprintf(out->user, sizeof(out->user), "%s", t->is_user ? t->user.name : "kernel");
    return 1;
}

static int64_t sys_chdir(uint64_t path_ptr) {
    char path[128], saved[256];
    if (copy_user_string(path, path_ptr, sizeof(path)) != 0) return SYSERR_FAULT;
    enter_cwd(saved, sizeof(saved));
    int ok = fat32_change_directory(path) == 0;
    if (ok) fat32_get_current_directory(P()->cwd, sizeof(P()->cwd));
    leave_cwd(saved);
    return ok ? 0 : SYSERR_NOENT;
}

static int64_t sys_getcwd(uint64_t buf, uint64_t size) {
    if (size == 0 || !user_range_ok(buf, size, 1)) return SYSERR_FAULT;
    k_snprintf((char *)buf, size, "%s", P()->cwd[0] ? P()->cwd : "/");
    return (int64_t)strlen((char *)buf);
}

/* kcommand(line): one of the kernel's built-in commands (ls, cat, ping, ...),
 * run in the caller's directory, as the caller's user. */
static int64_t sys_kcommand(uint64_t line_ptr) {
    char line[256], saved[256];
    if (copy_user_string(line, line_ptr, sizeof(line)) != 0) return SYSERR_FAULT;
    enter_cwd(saved, sizeof(saved));
    int known = shell_kernel_command(line);
    leave_cwd(saved);
    return known;
}

static int64_t sys_uname(uint64_t ptr) {
    if (!user_range_ok(ptr, sizeof(struct os_uname), 1)) return SYSERR_FAULT;
    struct os_uname *u = (struct os_uname *)ptr;
    k_snprintf(u->sysname, sizeof(u->sysname), "%s", OS_NAME);
    k_snprintf(u->release, sizeof(u->release), "%s", OS_VERSION);
    k_snprintf(u->hostname, sizeof(u->hostname), "%s", OS_HOSTNAME);
    return 0;
}
