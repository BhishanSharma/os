// process.c - user programs: loading ELF files, system calls, ending programs
//
// Every program is a task (sys/task.c) with its own page tables: the lower
// half holds its code, heap and stack, and the kernel (upper half) is mapped the
// same way in every address space, so a system call can read the program's
// memory directly once the pointer is checked (user_range_ok). Several
// programs can run at once; the timer switches between them. One of them may
// be in the foreground: it gets the keyboard and Ctrl+C.
#include "sys/process.h"
#include "sys/smp.h"
#include "sys/syscall_nums.h"
#include "sys/task.h"
#include "sys/users.h"
#include "sys/shell.h"
#include "sys/sysinfo.h"
#include "core/exceptions.h"
#include "drivers/paging.h"
#include "drivers/memory.h"
#include "drivers/heap.h"
#include "drivers/physmem.h"
#include "drivers/hda.h"
#include "drivers/fat32.h"
#include "drivers/fatvol.h"
#include "drivers/memory.h"
#include "net/tcp.h"
#include "net/net.h"
#include "sys/vfs.h"
#include "drivers/disk.h"
#include "drivers/mouse.h"
#include "lib/fbcon.h"
#include "drivers/keyboard.h"
#include "drivers/timer.h"
#include "drivers/rtc.h"
#include "lib/print.h"
#include "lib/string.h"
#include "sys/vm.h"
#include "sys/pipe.h"
#include "sys/port.h"
#include "drivers/registry.h"

#define USER_STACK_SIZE  (8ULL << 20)      /* the stack region; pages appear as it grows */
#define USER_HEAP_MAX    (64ULL << 30)     /* in practice: as much as RAM gives */
#define MAX_ARGS         16
#define MAX_FDS          32
#define MAX_PATH         256              /* FAT32_MAX_PATH: long names */
#define PAGE_UP(x)       (((x) + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1))
#define PAGE_DOWN(x)     ((x) & ~(uint64_t)(PAGE_SIZE - 1))

_Static_assert(KEYCODE_UP == KEY_UP_ARROW && KEYCODE_DOWN == KEY_DOWN_ARROW &&
               KEYCODE_LEFT == KEY_LEFT_ARROW && KEYCODE_RIGHT == KEY_RIGHT_ARROW,
               "user key codes must match the keyboard driver");

/* An open file: a file on disk, the console, or one end of a pipe. Shared
 * (counted) by every descriptor that refers to it: dup, fork, and the
 * standard 0-2 a program inherits from the one that started it. */
enum { F_FILE, F_CONSOLE, F_PIPE_R, F_PIPE_W };

typedef struct {
    int used, mode;                   // OPEN_READ / OPEN_WRITE / OPEN_APPEND
    int refs, kind;
    pipe_t *pipe;
    char path[MAX_PATH];
    uint8_t *data;
    uint32_t size, capacity, pos;
    vfs_file_t *stream;               // a big file opened for reading: read from disk as it goes
} file_t;

static file_t console_file = { .used = 1, .refs = 1 << 30, .kind = F_CONSOLE };

/* Files bigger than this are not read into memory at open() (a video). */
#define STREAM_MIN       (1024 * 1024)

typedef struct {
    uint64_t brk_start, brk;
    uint64_t stack_lo;                // the stack region's start
    uint64_t root;                    // the address space (shared by its threads)
    vm_space_t vm;
    int threads;                      // tasks running it
    port_space_t ports;
    uint64_t sig_handler[32], sig_tramp;
    uint32_t sig_pending;
    int no_echo;                      // CON_ECHO off: read(0) hides what is typed
    file_t *fds[MAX_FDS];
    char line_buf[256];               // keyboard input for read(0), one line at a time
    uint32_t line_len, line_pos;
    char cwd[256];                    // its own current directory
    int ctrlc_mode;                   // CTRLC_END / CTRLC_KEY
} process_t;

static int graphics_owner;            // pid drawing pixels (gfx), 0: the console
static int sound_owner;               // pid streaming sound (sys_sound), 0: none
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
        /* A page from all of RAM (drivers/physmem.h), or from the heap. */
        uint64_t phys = page_alloc();
        if (!phys) {
            void *frame = alloc_frame();
            if (!frame) return -1;
            phys = virt_to_phys(frame);
        }
        memset(page_address(phys), 0, PAGE_SIZE);
        if (paging_map_user(root, v, phys) != 0) {
            if (page_owned(phys)) page_free(phys);
            else free_frame(page_address(phys));
            return -1;
        }
    }
    return 0;
}

/* Copy into another address space through the kernel's mapping of its pages. */
static void copy_to_space(uint64_t root, uint64_t vaddr, const void *src, uint64_t len) {
    const uint8_t *s = src;
    while (len) {
        uint64_t page_off = vaddr & (PAGE_SIZE - 1);
        uint64_t n = PAGE_SIZE - page_off;
        if (n > len) n = len;
        uint64_t phys = paging_get_user_entry(root, vaddr) & ~0xFFFULL;
        memcpy((uint8_t *)page_address(phys) + page_off, s, n);
        vaddr += n;
        s += n;
        len -= n;
    }
}

/* 1 if the running program may read (or, with `write`, write) [ptr, ptr + len).
 * Pages of its regions that are not there yet are brought in (sys/vm.h), so
 * the kernel can then use the memory directly. */
static int user_range_ok(uint64_t ptr, uint64_t len, int write) {
    if (len == 0) return 1;
    if (ptr < USER_BASE || ptr + len > USER_TOP || ptr + len < ptr) return 0;
    process_t *p = P();
    uint64_t root = task_current()->root;
    for (uint64_t v = PAGE_DOWN(ptr); v < ptr + len; v += PAGE_SIZE) {
        uint64_t e = paging_get_user_entry(root, v);
        if ((e & PAGE_PRESENT) && (e & PAGE_USER) && (!write || (e & PAGE_RW))) continue;
        if (!p || vm_fault(root, &p->vm, v, write) != 0) return 0;
    }
    return 1;
}

/* A page fault in user mode: 0 if it was handled (the program goes on). */
int process_page_fault(uint64_t addr, uint64_t err) {
    task_t *t = task_current();
    process_t *p = t && t->is_user ? t->process : 0;
    if (!p || addr < USER_BASE || addr >= USER_TOP) return -1;
    return vm_fault(t->root, &p->vm, addr, (err & 2) != 0);
}

int process_signal_pending(void) {
    task_t *t = task_current();
    return t && t->is_user && t->process && ((process_t *)t->process)->sig_pending != 0;
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
 * The FAT32 driver has one current directory: the kernel shell's. A program
 * has its own, and its file system calls make every path absolute against it
 * (user_path) instead of switching the driver's: with several cores, another
 * task could switch it back while this one waits for the disk. Only kernel
 * commands (kcommand), which use the driver's directory, switch it. */

static const char *user_cwd(void) {
    return P()->cwd[0] ? P()->cwd : "/";
}

/* Copy the program's path string at `ptr` and make it absolute. 0 on success. */
static int user_path(uint64_t ptr, char *out, int size) {
    char raw[MAX_PATH];
    if (copy_user_string(raw, ptr, sizeof(raw)) != 0) return -1;
    return vfs_absolute(user_cwd(), raw, out, size) == 0 ? 0 : -1;
}

static void enter_cwd(char *saved, uint32_t size) {
    saved[0] = 0;
    fat32_get_current_directory(saved, size);
    if (fat32_change_directory(P()->cwd) != 0) fat32_change_directory("/");
}

static void leave_cwd(const char *saved) {
    if (saved[0]) fat32_change_directory(saved);
}

static void check_killed(void);

/* ---- Sockets ------------------------------------------------------------ */

static int own_socket(int s) { return tcp_owner(s) == task_current()->pid; }

/* A TCP result for the program: new sockets become its own; Ctrl+C ends it. */
static int64_t sock_result(int r, int is_new_socket) {
    if (r >= 0) {
        if (is_new_socket) tcp_set_owner(r, task_current()->pid);
        return r;
    }
    switch (r) {
        case TCP_ERR_INTR:    check_killed(); return SYSERR_TIMEOUT;
        case TCP_ERR_TIMEOUT: return SYSERR_TIMEOUT;
        case TCP_ERR_REFUSED: return SYSERR_REFUSED;
        case TCP_ERR_RESET:   return SYSERR_RESET;
        case TCP_ERR_CLOSED:  return SYSERR_CLOSED;
        case TCP_ERR_NOROUTE: return SYSERR_NOROUTE;
        case TCP_ERR_INUSE:   return SYSERR_INUSE;
        case TCP_ERR_NOSOCK:  return is_new_socket ? SYSERR_AGAIN : SYSERR_BADFD;
        default:              return SYSERR_IO;
    }
}

static int64_t sys_resolve(uint64_t host_ptr, uint64_t ip_ptr) {
    char host[128];
    if (copy_user_string(host, host_ptr, sizeof(host)) != 0 || !user_range_ok(ip_ptr, 4, 1)) return SYSERR_FAULT;
    if (!net_is_up()) return SYSERR_NOROUTE;
    uint8_t ip[4];
    if (net_resolve(host, ip) != 0) return SYSERR_NOHOST;
    memcpy((void *)ip_ptr, ip, 4);
    return 0;
}

static int64_t sys_connect(uint64_t packed, uint64_t port, uint64_t timeout_ms) {
    if (!net_is_up()) return SYSERR_NOROUTE;
    if (!port || port > 65535) return SYSERR_BADCALL;
    uint8_t ip[4] = { (uint8_t)(packed >> 24), (uint8_t)(packed >> 16), (uint8_t)(packed >> 8), (uint8_t)packed };
    return sock_result(tcp_connect(ip, (uint16_t)port, timeout_ms ? (uint32_t)timeout_ms : 10000), 1);
}

#define NO_LIMIT_MS 0x7FFFFFFu                        /* "no limit": about 37 hours */

static int64_t sys_accept(uint64_t s, uint64_t timeout_ms) {
    if (!own_socket((int)s)) return SYSERR_BADFD;
    uint32_t ms = timeout_ms && timeout_ms < NO_LIMIT_MS ? (uint32_t)timeout_ms : NO_LIMIT_MS;
    return sock_result(tcp_accept((int)s, ms), 1);
}

static int64_t sys_send(uint64_t s, uint64_t buf, uint64_t len) {
    if (!own_socket((int)s)) return SYSERR_BADFD;
    if (!user_range_ok(buf, len, 0)) return SYSERR_FAULT;
    if (len > 0x7FFFFFFF) len = 0x7FFFFFFF;
    return sock_result(tcp_send((int)s, (const void *)buf, (uint32_t)len), 0);
}

static int64_t sys_recv(uint64_t s, uint64_t buf, uint64_t len, uint64_t timeout_ms) {
    if (!own_socket((int)s)) return SYSERR_BADFD;
    if (!user_range_ok(buf, len, 1)) return SYSERR_FAULT;
    if (len > 0x7FFFFFFF) len = 0x7FFFFFFF;
    uint32_t ms = timeout_ms && timeout_ms < NO_LIMIT_MS ? (uint32_t)timeout_ms : NO_LIMIT_MS;
    return sock_result(tcp_recv((int)s, (void *)buf, (uint32_t)len, ms), 0);
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

static file_t *file_new(int kind) {
    file_t *f = kmalloc(sizeof(file_t));
    if (!f) return 0;
    memset(f, 0, sizeof(*f));
    f->used = 1;
    f->refs = 1;
    f->kind = kind;
    return f;
}

static int fd_free_slot(process_t *p, int from) {
    for (int i = from; i < MAX_FDS; i++)
        if (!p->fds[i]) return i;
    return -1;
}

static file_t *fd_get(uint64_t fd) {
    process_t *p = P();
    return p && fd < MAX_FDS ? p->fds[fd] : 0;
}

static int64_t close_file(file_t *f);

/* One reference less; the last one closes it. */
static int64_t file_put(file_t *f) {
    if (!f || f == &console_file) return 0;
    if (f->kind == F_PIPE_R || f->kind == F_PIPE_W) {        /* every reference is an end */
        if (--f->refs > 0) {
            pipe_end_closed(f->pipe, f->kind == F_PIPE_W);
            return 0;
        }
        return close_file(f);
    }
    if (--f->refs > 0) return 0;
    return close_file(f);
}

static file_t *file_get(file_t *f) {
    if (f && f != &console_file) f->refs++;
    return f;
}

static int64_t sys_open(uint64_t path_ptr, uint64_t mode) {
    char path[MAX_PATH];
    if (user_path(path_ptr, path, sizeof(path)) != 0) return SYSERR_FAULT;
    if (mode > OPEN_APPEND) return SYSERR_BADCALL;
    process_t *p = P();
    int fd = fd_free_slot(p, 3);
    if (fd < 0) return SYSERR_MFILE;

    file_t *f = file_new(F_FILE);
    if (!f) return SYSERR_NOMEM;
    f->mode = (int)mode;
    memcpy(f->path, path, sizeof(path));

    int64_t result = fd;
    if (mode != OPEN_READ && !user_may_write(path)) {
        result = SYSERR_PERM;
    } else {
        int exists = fat32_file_exists(path);
        if (mode == OPEN_READ || (mode == OPEN_APPEND && exists)) {
            uint32_t size = exists ? fat32_get_file_size(path) : 0xFFFFFFFF;
            if (size == 0xFFFFFFFF) {
                result = SYSERR_NOENT;
            } else if (mode == OPEN_READ && size > STREAM_MIN) {
                f->stream = kmalloc(sizeof(vfs_file_t));
                int r = f->stream ? vfs_open_file(path, f->stream) : SYSERR_NOMEM;
                if (r != 0) result = r == SYSERR_NOMEM ? SYSERR_NOMEM : r == FAT32_ERR_PERMISSION ? SYSERR_PERM : SYSERR_IO;
                f->size = size;
            } else if (file_grow(f, size + 1) != 0) {
                result = SYSERR_NOMEM;
            } else {
                int r = size ? fat32_read_file(path, f->data, size) : 0;
                if (r < 0) result = r == FAT32_ERR_PERMISSION ? SYSERR_PERM : SYSERR_IO;
                f->size = size;
            }
        }
    }
    if (result < 0) {
        kfree(f->data);
        if (f->stream) kfree(f->stream);
        kfree(f);
    } else {
        p->fds[fd] = f;
    }
    return result;
}

static int64_t close_file(file_t *f) {
    int64_t result = 0;
    if (f->kind == F_PIPE_R || f->kind == F_PIPE_W) {
        pipe_end_closed(f->pipe, f->kind == F_PIPE_W);
    } else if (f->mode != OPEN_READ) {              // f->path is absolute (sys_open)
        fat32_create_file(f->path);                 // fails harmlessly if it exists
        int r = fat32_write_file(f->path, f->data ? f->data : (const uint8_t *)"", f->size);
        if (r < 0) result = r == FAT32_ERR_PERMISSION ? SYSERR_PERM : SYSERR_IO;
    }
    kfree(f->data);
    if (f->stream) kfree(f->stream);
    kfree(f);
    return result;
}

static int64_t sys_close(uint64_t fd) {
    file_t *f = fd_get(fd);
    if (!f) return SYSERR_BADFD;
    P()->fds[fd] = 0;
    return file_put(f);
}

static int64_t sys_pipe(uint64_t ptr) {
    if (!user_range_ok(ptr, 2 * sizeof(int), 1)) return SYSERR_FAULT;
    process_t *p = P();
    int rfd = fd_free_slot(p, 0);
    if (rfd < 0) return SYSERR_MFILE;
    p->fds[rfd] = &console_file;                   /* (reserved while finding the second) */
    int wfd = fd_free_slot(p, 0);
    p->fds[rfd] = 0;
    if (wfd < 0) return SYSERR_MFILE;
    pipe_t *pp = pipe_new();
    file_t *r = file_new(F_PIPE_R), *w = file_new(F_PIPE_W);
    if (!pp || !r || !w) {
        kfree(pp);
        kfree(r);
        kfree(w);
        return SYSERR_NOMEM;
    }
    r->pipe = w->pipe = pp;
    r->mode = OPEN_READ;
    w->mode = OPEN_WRITE;
    p->fds[rfd] = r;
    p->fds[wfd] = w;
    ((int *)ptr)[0] = rfd;
    ((int *)ptr)[1] = wfd;
    return 0;
}

static int64_t sys_dup2(uint64_t fd, uint64_t to) {
    file_t *f = fd_get(fd);
    if (!f || to >= MAX_FDS) return SYSERR_BADFD;
    process_t *p = P();
    if (p->fds[to] == f) return (int64_t)to;
    file_put(p->fds[to]);
    p->fds[to] = file_get(f);
    if (f->kind == F_PIPE_R || f->kind == F_PIPE_W) pipe_end_added(f->pipe, f->kind == F_PIPE_W);
    return (int64_t)to;
}

static int64_t sys_dup(uint64_t fd) {
    if (!fd_get(fd)) return SYSERR_BADFD;
    int to = fd_free_slot(P(), 0);
    return to < 0 ? SYSERR_MFILE : sys_dup2(fd, (uint64_t)to);
}

/* ---- Ending a program -------------------------------------------------- */

/* End this thread; with `whole`, the whole program (every thread of it).
 * The last thread to go saves its files, frees its memory, and becomes the
 * zombie its parent collects. */
static void __attribute__((noreturn)) finish(int code, int whole) {
    task_t *t = task_current();
    process_t *p = t->process;
    if (p && whole)                                     /* the other threads stop too */
        for (int i = 0; i < MAX_TASKS; i++) {
            task_t *o = task_at(i);
            if (o != t && o->process == p && o->state != TASK_FREE && o->state != TASK_ZOMBIE && !o->kill_code) {
                o->kill_code = code ? code : EXIT_KILLED;
                task_interrupt(o);
            }
        }
    if (p && --p->threads > 0) {                        /* not the last: the program goes on */
        t->process = 0;
        t->root = paging_kernel_root();
        paging_switch(t->root);
        task_exit(code);
    }
    process_reap_orphans();                             /* threads that ended before it */
    tcp_close_owned_by(t->pid);                         /* its connections end with it */
    if (sound_owner == t->pid) {                        /* and its sound */
        sound_stream_close();
        sound_owner = 0;
    }
    uint64_t root = t->root;
    if (p) {
        for (int i = 0; i < MAX_FDS; i++) file_put(p->fds[i]);
        port_space_free(&p->ports);
        vm_free(&p->vm);
        root = p->root;
        task_t *parent = task_by_pid(t->parent);        /* SIGCHLD, if it asked for it */
        process_t *pp = parent && parent->is_user ? parent->process : 0;
        if (pp && pp->sig_handler[SIGCHLD]) {
            pp->sig_pending |= 1u << SIGCHLD;
            task_interrupt(parent);
        }
        kfree(p);
        t->process = 0;
    }
    t->root = paging_kernel_root();
    paging_switch(t->root);
    paging_free_address_space(root);
    if (mouse_owner() == t->pid) mouse_set_owner(0);  // the console gets the mouse back
    if (graphics_owner == t->pid) {                    // and the screen
        graphics_owner = 0;
        print_set_graphics(0);
    }
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

static void __attribute__((noreturn)) process_exit(int code) {
    finish(code, 1);
}

int process_interrupted(void) {
    task_t *t = task_current();
    if (t && t->is_user) {
        if (!t->kill_code && keyboard_ctrl_c && t->pid == foreground_pid) {
            keyboard_ctrl_c = 0;
            t->kill_code = EXIT_INTERRUPTED;
        }
        return t->kill_code != 0;
    }
    return keyboard_ctrl_c;                    /* a kernel command (download) */
}

/* Called where a program can be stopped safely: Ctrl+C, `kill`. A program
 * with a SIGINT handler gets Ctrl+C as that signal instead. */
static void check_killed(void) {
    task_t *t = task_current();
    process_t *p = t->is_user ? t->process : 0;
    if (p && t->kill_code == EXIT_INTERRUPTED && p->sig_handler[SIGINT]) {
        t->kill_code = 0;
        p->sig_pending |= 1u << SIGINT;
        return;
    }
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
    process_t *p = t->process;
    int overflow = p && address < p->stack_lo && address + (1u << 20) >= p->stack_lo;
    if (vector == 14)
        k_snprintf(msg, sizeof(msg), "%s (pid %d): %s at address 0x%lx (rip 0x%lx) - program terminated",
                   t->name, t->pid,
                   overflow ? "Stack overflow (more than 8 MiB of stack)"
                   : address < USER_BASE || address >= USER_TOP ? "Segmentation fault (kernel or unmapped memory)"
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
    bkl_enter();
    check_killed();
    task_preempt();
    bkl_leave();
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

/* Shift+Up/Down scroll the console for every program; they never see them. */
static int scroll_key(int c) {
    if (c == KEY_SCROLL_UP) scroll_up_lines(1);
    else if (c == KEY_SCROLL_DOWN) scroll_down_lines(1);
    else return 0;
    return 1;
}

static void read_keyboard_line(process_t *p) {
    p->line_len = p->line_pos = 0;
    while (1) {
        wait_for_foreground();
        check_killed();
        if (p->sig_pending) return;                 /* read() returns 0: the handler runs */
        int c = get_char();
        if (!c || scroll_key(c)) {
            if (!c) task_sleep(10);
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
                if (!p->no_echo) print_str("\b \b");
            }
            continue;
        }
        if (c >= 32 && c < 127 && p->line_len < sizeof(p->line_buf) - 1) {
            p->line_buf[p->line_len++] = (char)c;
            if (!p->no_echo) print_char((char)c);
        }
    }
}

/* ---- System calls ------------------------------------------------------ */

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    if (!user_range_ok(buf, len, 0)) return SYSERR_FAULT;
    const char *src = (const char *)buf;
    file_t *f = fd_get(fd);
    if (!f) return SYSERR_BADFD;
    if (f->kind == F_CONSOLE) {
        print_batch_begin();
        for (uint64_t i = 0; i < len; i++) print_char(src[i]);
        print_batch_end();
        return (int64_t)len;
    }
    if (f->kind == F_PIPE_W) {
        int64_t n = pipe_write(f->pipe, src, len);
        if (n == -2) {                                  /* nobody reads: SIGPIPE ends it */
            process_t *p = P();
            if (!p->sig_handler[SIGPIPE]) process_exit(128 + SIGPIPE);
            p->sig_pending |= 1u << SIGPIPE;
            return SYSERR_PIPE;
        }
        return n < 0 ? SYSERR_TIMEOUT : n;
    }
    if (f->kind != F_FILE || f->mode == OPEN_READ) return SYSERR_BADFD;
    if (len > 64 * 1024 * 1024 || file_grow(f, f->size + (uint32_t)len) != 0) return SYSERR_NOMEM;
    memcpy(f->data + f->size, src, len);
    f->size += (uint32_t)len;
    return (int64_t)len;
}

static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len) {
    if (!user_range_ok(buf, len, 1)) return SYSERR_FAULT;
    char *dst = (char *)buf;
    process_t *p = P();
    file_t *f = fd_get(fd);
    if (!f) return SYSERR_BADFD;
    if (f->kind == F_CONSOLE) {
        if (len == 0) return 0;
        if (p->line_pos >= p->line_len) read_keyboard_line(p);
        uint64_t n = 0;
        while (n < len && p->line_pos < p->line_len) dst[n++] = p->line_buf[p->line_pos++];
        return (int64_t)n;
    }
    if (f->kind == F_PIPE_R) {
        int64_t n = pipe_read(f->pipe, dst, len);
        if (n < 0) check_killed();
        return n < 0 ? SYSERR_TIMEOUT : n;
    }
    if (f->kind != F_FILE || f->mode != OPEN_READ) return SYSERR_BADFD;
    uint64_t n = f->size - f->pos;
    if (n > len) n = len;
    if (f->stream) {                                        /* from the disk, as it goes */
        int r = n ? vfs_read_at(f->stream, f->pos, dst, (uint32_t)n) : 0;
        if (r < 0) return SYSERR_IO;
        n = (uint64_t)r;
    } else {
        memcpy(dst, f->data + f->pos, n);
    }
    f->pos += (uint32_t)n;
    return (int64_t)n;
}

static int64_t sys_seek(uint64_t fd, int64_t offset, uint64_t whence) {
    file_t *f = fd_get(fd);
    if (!f || f->kind != F_FILE) return SYSERR_BADFD;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f->pos : whence == SEEK_END ? (int64_t)f->size : -1;
    if (base < 0) return SYSERR_BADCALL;
    int64_t at = base + offset;
    if (at < 0) at = 0;
    if (at > (int64_t)f->size) at = f->size;
    f->pos = (uint32_t)at;
    return at;
}

/* ---- Sound: one program at a time streams to the speakers ---------------------- */


static int64_t sys_sound(uint64_t op, uint64_t a, uint64_t b) {
    int pid = task_current()->pid;
    if (op == SOUND_OPEN) {
        if (sound_owner && sound_owner != pid) return SYSERR_INUSE;
        if (sound_owner == pid) return 0;
        int r = sound_stream_open();
        if (r == -2) return SYSERR_INUSE;
        if (r != 0) return SYSERR_NOENT;
        sound_owner = pid;
        return 0;
    }
    if (sound_owner != pid) return SYSERR_BADFD;
    if (op == SOUND_WRITE) {
        uint64_t count = b;
        if (count > (1u << 20) || !user_range_ok(a, count * 4, 0)) return SYSERR_FAULT;
        const int16_t *frames = (const int16_t *)a;
        uint64_t done = 0;
        while (done < count) {
            done += (uint64_t)sound_stream_write(frames + 2 * done, (int)(count - done));
            if (done < count) {
                if (process_interrupted()) break;
                cpu_wait();                                 /* room comes as it plays */
            }
        }
        return (int64_t)done;
    }
    if (op == SOUND_QUEUED) return sound_stream_queued();
    if (op == SOUND_CLOSE) {
        sound_stream_close();
        sound_owner = 0;
        return 0;
    }
    return SYSERR_BADCALL;
}

static vm_region_t *heap_region(process_t *p) {
    for (vm_region_t *r = p->vm.regions; r; r = r->next)
        if (r->start == p->brk_start && r->kind == VM_ANON) return r;
    return 0;
}

static int64_t sys_sbrk(int64_t increment) {
    process_t *p = P();
    uint64_t old = p->brk, new_brk = p->brk + (uint64_t)increment;
    vm_region_t *heap = heap_region(p);
    if (!heap || new_brk < p->brk_start || new_brk > p->brk_start + USER_HEAP_MAX ||
        PAGE_UP(new_brk) > p->vm.mmap_next - PAGE_SIZE)
        return SYSERR_NOMEM;
    if (PAGE_UP(new_brk) > heap->end) heap->end = PAGE_UP(new_brk);   /* pages come when touched */
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

static int64_t sys_time(uint64_t ptr, uint64_t utc) {
    if (!user_range_ok(ptr, sizeof(struct os_time), 1)) return SYSERR_FAULT;
    rtc_time_t t;
    if (rtc_read(&t) != 0) return SYSERR_IO;
    if (!utc) rtc_add_minutes(&t, RTC_LOCAL_OFFSET_MIN);
    struct os_time *out = (struct os_time *)ptr;
    out->year = t.year; out->month = t.month; out->day = t.day;
    out->hour = t.hour; out->minute = t.minute; out->second = t.second;
    k_snprintf(out->zone, sizeof(out->zone), "%s", utc ? "UTC" : RTC_LOCAL_TZ_NAME);
    return 0;
}

/* With `wait`, block until there is a key (and this program has the keyboard). */
static int64_t sys_getkey(uint64_t wait) {
    int c;
    while (1) {
        if (wait) wait_for_foreground();
        else if (foreground_pid != task_current()->pid) return 0;
        c = get_char();
        if (scroll_key(c)) continue;
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
        case CON_ECHO:   P()->no_echo = a == 0; return 0;
        default:         return SYSERR_BADCALL;
    }
}

/* A file system result for the program: FATV_* codes become SYSERR_*. */
static int64_t fs_result(int r) {
    if (r >= 0) return r;
    switch (r) {
        case FATV_NOT_FOUND:       return SYSERR_NOENT;
        case FATV_EXISTS:          return SYSERR_EXIST;
        case FATV_BAD_NAME:        return SYSERR_BADNAME;
        case FATV_NOT_EMPTY:       return SYSERR_NOTEMPTY;
        case FATV_FULL:            return SYSERR_NOSPC;
        case FATV_READ_ONLY:       return SYSERR_ROFS;
        case FAT32_ERR_PERMISSION: return SYSERR_PERM;
        default:                   return SYSERR_IO;
    }
}

#define LISTDIR_MAX 1024

static int64_t sys_readdir(uint64_t index, uint64_t ptr) {
    if (!user_range_ok(ptr, sizeof(struct os_dirent), 1)) return SYSERR_FAULT;
    if (index >= LISTDIR_MAX) return 0;
    fat32_file_info_t *list = kmalloc((index + 1) * sizeof(fat32_file_info_t));
    if (!list) return SYSERR_NOMEM;
    int count = fat32_list_directory_ex(user_cwd(), list, (uint32_t)index + 1);
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
    if (user_path(path_ptr, path, sizeof(path)) != 0) return SYSERR_FAULT;
    return fat32_file_exists(path) ? fs_result(fat32_delete_file(path)) : SYSERR_NOENT;
}

static int64_t sys_mkdir(uint64_t path_ptr) {
    char path[MAX_PATH];
    if (user_path(path_ptr, path, sizeof(path)) != 0) return SYSERR_FAULT;
    return fs_result(fat32_mkdir(path));
}

static int64_t sys_stat(uint64_t path_ptr, uint64_t ptr) {
    char path[MAX_PATH];
    if (user_path(path_ptr, path, sizeof(path)) != 0) return SYSERR_FAULT;
    if (!user_range_ok(ptr, sizeof(struct os_stat), 1)) return SYSERR_FAULT;
    struct os_stat *out = (struct os_stat *)ptr;
    fat32_file_info_t one;
    int64_t result = 0;
    if (fat32_list_directory_ex(path, &one, 1) >= 0) {        /* a folder */
        out->is_dir = 1;
        out->size = 0;
    } else if (fat32_file_exists(path)) {
        out->is_dir = 0;
        out->size = fat32_get_file_size(path);
    } else {
        result = SYSERR_NOENT;
    }
    return result;
}

/* The whole folder at once (with long names): path 0 is the current one. */
static int64_t sys_listdir(uint64_t path_ptr, uint64_t buf, uint64_t max) {
    char path[MAX_PATH];
    if (!path_ptr) kstrncpy(path, user_cwd(), sizeof(path));
    else if (user_path(path_ptr, path, sizeof(path)) != 0) return SYSERR_FAULT;
    if (max > LISTDIR_MAX) max = LISTDIR_MAX;
    if (!max) return 0;
    if (!user_range_ok(buf, max * sizeof(struct os_dirent), 1)) return SYSERR_FAULT;
    fat32_file_info_t *list = kmalloc(max * sizeof(fat32_file_info_t));
    if (!list) return SYSERR_NOMEM;
    int count = fat32_list_directory_ex(path, list, (uint32_t)max);
    struct os_dirent *out = (struct os_dirent *)buf;
    for (int i = 0; i < count; i++) {
        k_snprintf(out[i].name, sizeof(out[i].name), "%s", list[i].name);
        out[i].is_dir = list[i].is_directory;
        out[i].size = list[i].size;
    }
    kfree(list);
    return count < 0 ? SYSERR_NOENT : count;
}

static int64_t sys_meminfo(uint64_t ptr) {
    if (!user_range_ok(ptr, sizeof(struct os_meminfo), 1)) return SYSERR_FAULT;
    struct os_meminfo *m = (struct os_meminfo *)ptr;
    m->ram = get_total_memory();
    m->heap_total = heap_get_total();
    m->heap_used = heap_get_used();
    m->heap_blocks = heap_get_allocations();
    m->pages_total = physmem_total();
    m->pages_used = physmem_used();
    return 0;
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
static int64_t sys_mouse(uint64_t ptr);
static int64_t sys_gfx(uint64_t op, uint64_t arg);
static int64_t sys_fork(struct exc_frame *f);
static int64_t sys_exec(struct exc_frame *f, uint64_t path_ptr, uint64_t argv_ptr);
static int64_t sys_thread(uint64_t entry, uint64_t arg, uint64_t stack);
static int64_t sys_futex(uint64_t op, uint64_t addr, uint64_t value);
static int64_t sys_mmap(uint64_t len, uint64_t prot, int64_t fd);
static int64_t sys_shmmap(uint64_t name_ptr, uint64_t size);
static int64_t sys_port(struct exc_frame *f);
static int64_t sys_signal(uint64_t sig, uint64_t handler, uint64_t tramp);
static int64_t sys_sigsend(int64_t pid, uint64_t sig);
static void sys_sigreturn(struct exc_frame *f, uint64_t ptr);
static void deliver_signals(struct exc_frame *f);
static int64_t sys_registry(uint64_t index, uint64_t ptr);
static int64_t sys_auth(uint64_t name_ptr, uint64_t pass_ptr, uint64_t out_ptr);

/* int 0x80 (usermode.asm). Runs with interrupts on, so a blocking call
 * (keyboard, sleep) still gets timer and keyboard interrupts. */
static void syscall_dispatch_locked(struct exc_frame *f);

/* Every system call runs under the big kernel lock (sys/smp.h). */
void syscall_dispatch(struct exc_frame *f) {
    bkl_enter();
    syscall_dispatch_locked(f);
    bkl_leave();
}

static void syscall_dispatch_locked(struct exc_frame *f) {
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
        case SYS_TIME:    r = sys_time(f->rdi, f->rsi); break;
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
        case SYS_MOUSE:   r = sys_mouse(f->rdi); break;
        case SYS_GFX:     r = sys_gfx(f->rdi, f->rsi); break;
        case SYS_RESOLVE: r = sys_resolve(f->rdi, f->rsi); break;
        case SYS_CONNECT: r = sys_connect(f->rdi, f->rsi, f->rdx); break;
        case SYS_LISTEN:  r = sock_result(tcp_listen((uint16_t)f->rdi), 1); break;
        case SYS_ACCEPT:  r = sys_accept(f->rdi, f->rsi); break;
        case SYS_SEND:    r = sys_send(f->rdi, f->rsi, f->rdx); break;
        case SYS_RECV:    r = sys_recv(f->rdi, f->rsi, f->rdx, f->r10); break;
        case SYS_SOCKCLOSE: r = own_socket((int)f->rdi) ? (tcp_close((int)f->rdi), 0) : SYSERR_BADFD; break;
        case SYS_SEEK:    r = sys_seek(f->rdi, (int64_t)f->rsi, f->rdx); break;
        case SYS_SOUND:   r = sys_sound(f->rdi, f->rsi, f->rdx); break;
        case SYS_MKDIR:   r = sys_mkdir(f->rdi); break;
        case SYS_STAT:    r = sys_stat(f->rdi, f->rsi); break;
        case SYS_LISTDIR: r = sys_listdir(f->rdi, f->rsi, f->rdx); break;
        case SYS_MEMINFO: r = sys_meminfo(f->rdi); break;
        case SYS_MMAP:    r = sys_mmap(f->rdi, f->rsi, (int64_t)f->rdx); break;
        case SYS_MUNMAP:  r = vm_unmap(P()->root, &P()->vm, f->rdi) == 0 ? 0 : SYSERR_BADCALL; break;
        case SYS_SHMMAP:  r = sys_shmmap(f->rdi, f->rsi); break;
        case SYS_FORK:    r = sys_fork(f); break;
        case SYS_EXEC:    r = sys_exec(f, f->rdi, f->rsi); if (r == 0) return; break;
        case SYS_THREAD:  r = sys_thread(f->rdi, f->rsi, f->rdx); break;
        case SYS_THREAD_EXIT: finish((int)(int32_t)f->rdi, 0);
        case SYS_FUTEX:   r = sys_futex(f->rdi, f->rsi, f->rdx); break;
        case SYS_PORT_CREATE: case SYS_PORT_SEND: case SYS_PORT_RECEIVE:
        case SYS_PORT_REGISTER: case SYS_PORT_LOOKUP: case SYS_PORT_CLOSE:
                          r = sys_port(f); break;
        case SYS_PIPE:    r = sys_pipe(f->rdi); break;
        case SYS_DUP:     r = sys_dup(f->rdi); break;
        case SYS_DUP2:    r = sys_dup2(f->rdi, f->rsi); break;
        case SYS_SIGNAL:  r = sys_signal(f->rdi, f->rsi, f->rdx); break;
        case SYS_SIGSEND: r = sys_sigsend((int64_t)f->rdi, f->rsi); break;
        case SYS_SIGRETURN: sys_sigreturn(f, f->rdi); deliver_signals(f); return;
        case SYS_REGISTRY: r = sys_registry(f->rdi, f->rsi); break;
        case SYS_AUTH:    r = sys_auth(f->rdi, f->rsi, f->rdx); break;
        default:          r = SYSERR_BADCALL; break;
    }
    f->rax = (uint64_t)r;
    deliver_signals(f);
}

/* ---- Starting programs -------------------------------------------------- */

const char *process_error_text(int err) {
    switch (err) {
        case PROC_ERR_NOT_FOUND: return "no such program";
        case PROC_ERR_NOT_ELF:   return "not an x86_64 ELF executable for this OS";
        case PROC_ERR_NO_MEMORY: return "out of memory";
        case PROC_ERR_BUSY:      return "too many programs running";
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
static int spawn_as(const uint8_t *data, uint32_t size, int argc, char **argv, const user_t *user,
                    const char *cwd, process_t *parent) {
    if (argc > MAX_ARGS) argc = MAX_ARGS;
    process_reap_orphans();                            /* make room: free ended threads */

    process_t *p = kmalloc(sizeof(process_t));
    uint64_t root = paging_create_address_space();
    if (!p || !root) {
        kfree(p);
        if (root) paging_free_address_space(root);
        return PROC_ERR_NO_MEMORY;
    }
    memset(p, 0, sizeof(*p));

    vm_init_space(&p->vm);
    uint64_t image_end = 0, entry = load_elf(root, data, size, &image_end), sp = 0;
    int err = entry ? 0 : PROC_ERR_NOT_ELF;
    p->stack_lo = USER_TOP - USER_STACK_SIZE;
    if (!err && (!vm_add(&p->vm, p->stack_lo, USER_TOP, VM_READ | VM_WRITE, VM_ANON, 0) ||
                 !vm_add(&p->vm, image_end, image_end, VM_READ | VM_WRITE, VM_ANON, 0) ||
                 map_user_range(root, USER_TOP - PAGE_SIZE, USER_TOP) != 0))
        err = PROC_ERR_NO_MEMORY;
    if (!err && !(sp = build_stack(root, argc, argv))) err = PROC_ERR_NO_MEMORY;
    if (err) {
        vm_free(&p->vm);
        paging_free_address_space(root);
        kfree(p);
        return err;
    }
    p->brk_start = p->brk = image_end;
    p->root = root;
    p->threads = 1;
    for (int i = 0; i < 3; i++) {                       /* 0-2 from whoever started it */
        file_t *f = parent && parent->fds[i] ? parent->fds[i] : &console_file;
        p->fds[i] = f;
        file_get(f);
        if (f->kind == F_PIPE_R || f->kind == F_PIPE_W) pipe_end_added(f->pipe, f->kind == F_PIPE_W);
    }
    kstrncpy(p->cwd, cwd, sizeof(p->cwd));

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
        for (int i = 0; i < 3; i++) file_put(p->fds[i]);
        vm_free(&p->vm);
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
    char cwd[MAX_PATH];
    fat32_get_current_directory(cwd, sizeof(cwd));        // the kernel shell's
    return spawn_as(data, size, argc, argv, user_current(), cwd, 0);
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
        if (t->state == TASK_ZOMBIE && t->is_user && (t->parent == 1 || t->is_thread) && t != task_current())
            task_reap(t);                              /* nobody waits for a thread: free it here */
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

/* `name` in folder `dir` (or as given, if it is absolute), else in the system folder. */
static uint8_t *find_program_from(const char *dir, const char *name, uint32_t *size) {
    char path[MAX_PATH];
    if (vfs_absolute(dir, name, path, sizeof(path)) != 0) return 0;
    uint8_t *data = read_program_file(path, size);
    int has_folder = 0;
    for (const char *c = name; *c; c++)
        if (*c == '/') has_folder = 1;
    if (data || has_folder) return data;
    /* Not here: the system folder (the RAM disk's programs). */
    k_snprintf(path, sizeof(path), "%s/%s", strcmp(vfs_system_dir(), "/") ? vfs_system_dir() : "", name);
    return read_program_file(path, size);
}

uint8_t *process_find_program(const char *name, uint32_t *size) {
    char cwd[MAX_PATH];
    fat32_get_current_directory(cwd, sizeof(cwd));
    return find_program_from(cwd, name, size);
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
    char path[MAX_PATH];
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

    uint32_t size = 0;
    // As another user (su): only system programs, never one from this folder.
    uint8_t *image = find_program_from(user_ptr ? "/" : user_cwd(), path, &size);
    char cwd[MAX_PATH];
    kstrncpy(cwd, user_cwd(), sizeof(cwd));
    int64_t result = image ? spawn_as(image, size, argc, argv, &as, cwd, P()) : PROC_ERR_NOT_FOUND;
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
            if (pid == -1 && t->is_thread) continue;
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
    task_interrupt(t);
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
    char path[MAX_PATH];
    if (user_path(path_ptr, path, sizeof(path)) != 0) return SYSERR_FAULT;
    return vfs_folder(path, P()->cwd, sizeof(P()->cwd)) == 0 ? 0 : SYSERR_NOENT;
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

/* getmouse(): the pointer for this program, which takes the mouse from the
 * console (selection, paste, wheel scrolling) while it is in the foreground. */
static int64_t sys_mouse(uint64_t ptr) {
    if (!user_range_ok(ptr, sizeof(struct os_mouse), 1)) return SYSERR_FAULT;
    struct os_mouse *m = (struct os_mouse *)ptr;
    if (!mouse_present()) {
        memset(m, 0, sizeof(*m));
        return 0;
    }
    mouse_set_owner(task_current()->pid);
    mouse_read(&m->col, &m->row, &m->buttons, &m->wheel);
    return 1;
}

/* gfx(op, arg): pixels for programs such as the image viewer. */
static int64_t sys_gfx(uint64_t op, uint64_t arg) {
    uint32_t width, height, cell_w, cell_h, grid_x, grid_y;
    fbcon_geometry(&width, &height, &cell_w, &cell_h, &grid_x, &grid_y);
    if (!width || !fbcon_active()) return SYSERR_NOENT;            // VGA text mode
    uint32_t top = grid_y + (uint32_t)print_status_rows() * cell_h;
    int me = task_current()->pid;

    switch (op) {
        case GFX_INFO: {
            if (!user_range_ok(arg, sizeof(struct os_gfx_info), 1)) return SYSERR_FAULT;
            struct os_gfx_info *info = (struct os_gfx_info *)arg;
            info->width = (int)width;
            info->height = (int)height;
            info->top = (int)top;
            info->cell_w = (int)cell_w;
            info->cell_h = (int)cell_h;
            return 0;
        }
        case GFX_BEGIN:
            if (foreground_pid != me) return SYSERR_PERM;
            if (graphics_owner && graphics_owner != me) return SYSERR_AGAIN;
            graphics_owner = me;
            print_set_graphics(1);
            fbcon_fill(0, top, width, height - top, 0);
            return 0;
        case GFX_BLIT: {
            if (graphics_owner != me) return SYSERR_PERM;
            if (!user_range_ok(arg, sizeof(struct os_blit), 0)) return SYSERR_FAULT;
            struct os_blit b = *(const struct os_blit *)arg;
            if (b.w <= 0 || b.h <= 0 || b.stride < b.w || b.w > 16384 || b.h > 16384) return SYSERR_BADCALL;
            uint64_t bytes = ((uint64_t)(b.h - 1) * (uint64_t)b.stride + (uint64_t)b.w) * 4;
            if (!user_range_ok((uint64_t)(uintptr_t)b.pixels, bytes, 0)) return SYSERR_FAULT;
            // Keep off the status bar: skip rows above `top`.
            if (b.y < (int)top) {
                int skip = (int)top - b.y;
                if (skip >= b.h) return 0;
                b.pixels += (uint64_t)skip * (uint64_t)b.stride;
                b.h -= skip;
                b.y = (int)top;
            }
            fbcon_blit(b.x, b.y, b.w, b.h, (const uint32_t *)b.pixels, b.stride);
            return 0;
        }
        case GFX_END:
            if (graphics_owner != me) return SYSERR_PERM;
            graphics_owner = 0;
            print_set_graphics(0);
            return 0;
        default:
            return SYSERR_BADCALL;
    }
}

/* ---- Memory (stage 1) --------------------------------------------------------------- */

static int64_t sys_mmap(uint64_t len, uint64_t prot, int64_t fd) {
    process_t *p = P();
    int vp = VM_READ | ((prot & PROT_WRITE) ? VM_WRITE : 0);
    if (fd < 0) {
        uint64_t a = vm_map(&p->vm, len, vp, VM_ANON, 0);
        return a ? (int64_t)a : SYSERR_NOMEM;
    }
    file_t *f = fd_get((uint64_t)fd);
    if (!f || f->kind != F_FILE || f->mode != OPEN_READ) return SYSERR_BADFD;
    vm_object_t *o = vm_file_object(f->path);
    if (!o) return SYSERR_IO;
    uint64_t a = vm_map(&p->vm, len ? len : f->size, vp, VM_FILE, o);
    return a ? (int64_t)a : SYSERR_NOMEM;
}

static int64_t sys_shmmap(uint64_t name_ptr, uint64_t size) {
    char name[32];
    if (copy_user_string(name, name_ptr, sizeof(name)) != 0) return SYSERR_FAULT;
    vm_object_t *o = vm_shm_object(name, size);
    if (!o) return SYSERR_NOMEM;
    uint64_t a = vm_map(&P()->vm, size, VM_READ | VM_WRITE, VM_SHM, o);
    return a ? (int64_t)a : SYSERR_NOMEM;
}

/* ---- fork and exec (stage 4) -------------------------------------------------------- */

static int64_t sys_fork(struct exc_frame *f) {
    process_t *p = P();
    task_t *me = task_current();
    process_t *c = kmalloc(sizeof(process_t));
    uint64_t root = paging_create_address_space();
    if (!c || !root) {
        kfree(c);
        if (root) paging_free_address_space(root);
        return SYSERR_NOMEM;
    }
    memcpy(c, p, sizeof(*c));
    memset(&c->ports, 0, sizeof(c->ports));
    c->sig_pending = 0;
    c->threads = 1;
    c->root = root;
    if (vm_fork(p->root, &p->vm, root, &c->vm) != 0) {
        vm_free(&c->vm);
        paging_free_address_space(root);
        kfree(c);
        return SYSERR_NOMEM;
    }
    for (int i = 0; i < MAX_FDS; i++) {
        file_t *fl = c->fds[i];
        if (!fl) continue;
        file_get(fl);
        if (fl->kind == F_PIPE_R || fl->kind == F_PIPE_W) pipe_end_added(fl->pipe, fl->kind == F_PIPE_W);
    }
    struct exc_frame child = *f;
    child.rax = 0;                                      /* fork() returns 0 in the child */
    task_t *t = task_create_frame(me->name, root, &child);
    if (!t) {
        for (int i = 0; i < MAX_FDS; i++) file_put(c->fds[i]);
        vm_free(&c->vm);
        paging_free_address_space(root);
        kfree(c);
        return SYSERR_AGAIN;
    }
    t->process = c;
    t->user = me->user;
    return t->pid;
}

/* Replace this program with another; its descriptors and folder stay. On
 * success the frame starts the new program (0 is returned, and not used). */
static int64_t sys_exec(struct exc_frame *f, uint64_t path_ptr, uint64_t argv_ptr) {
    process_t *p = P();
    task_t *me = task_current();
    if (p->threads > 1) return SYSERR_AGAIN;
    char path[MAX_PATH];
    if (copy_user_string(path, path_ptr, sizeof(path)) != 0) return SYSERR_FAULT;
    char *strings = kmalloc(1024), *argv[MAX_ARGS];
    if (!strings) return SYSERR_NOMEM;
    int argc = 0;
    size_t used = 0;
    for (; argv_ptr && argc < MAX_ARGS; argc++) {
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
    if (argc == 0) argv[argc++] = path;
    uint32_t size = 0;
    uint8_t *image = find_program_from(user_cwd(), path, &size);
    if (!image) {
        kfree(strings);
        return SYSERR_NOENT;
    }
    uint64_t root = paging_create_address_space();
    vm_space_t vm;
    vm_init_space(&vm);
    uint64_t image_end = 0, entry = root ? load_elf(root, image, size, &image_end) : 0, sp = 0;
    uint64_t stack_lo = USER_TOP - USER_STACK_SIZE;
    int ok = entry && vm_add(&vm, stack_lo, USER_TOP, VM_READ | VM_WRITE, VM_ANON, 0) &&
             vm_add(&vm, image_end, image_end, VM_READ | VM_WRITE, VM_ANON, 0) &&
             map_user_range(root, USER_TOP - PAGE_SIZE, USER_TOP) == 0 &&
             (sp = build_stack(root, argc, argv)) != 0;
    kfree(image);
    kfree(strings);
    if (!ok) {
        vm_free(&vm);
        if (root) paging_free_address_space(root);
        return entry ? SYSERR_NOMEM : SYSERR_NOEXEC;
    }
    uint64_t old = p->root;
    vm_free(&p->vm);
    p->vm = vm;
    p->root = me->root = root;
    p->brk_start = p->brk = image_end;
    p->stack_lo = stack_lo;
    memset(p->sig_handler, 0, sizeof(p->sig_handler));
    p->sig_pending = 0;
    paging_switch(root);
    paging_free_address_space(old);
    memset(f, 0, sizeof(*f));
    f->rip = entry;
    f->cs = 0x2B;
    f->rflags = 0x202;
    f->rsp = sp;
    f->ss = 0x33;
    return 0;
}

/* ---- Threads (stage 2) -------------------------------------------------------------- */

static int64_t sys_thread(uint64_t entry, uint64_t arg, uint64_t stack) {
    process_t *p = P();
    task_t *me = task_current();
    if (entry < USER_BASE || entry >= USER_TOP || stack < USER_BASE || stack > USER_TOP) return SYSERR_FAULT;
    process_reap_orphans();
    struct exc_frame tf;
    memset(&tf, 0, sizeof(tf));
    tf.rip = entry;
    tf.cs = 0x2B;
    tf.rflags = 0x202;
    tf.rsp = (stack & ~(uint64_t)15) - 8;               /* as if called: entry sees rsp = 8 mod 16 */
    tf.ss = 0x33;
    tf.rdi = arg;
    task_t *t = task_create_frame(me->name, p->root, &tf);
    if (!t) return SYSERR_AGAIN;
    t->process = p;
    t->user = me->user;
    t->is_thread = 1;
    p->threads++;
    return t->pid;
}

static int64_t sys_futex(uint64_t op, uint64_t addr, uint64_t value) {
    if ((addr & 3) || !user_range_ok(addr, 4, 0)) return SYSERR_FAULT;
    const void *chan = (const void *)(P()->root ^ addr);
    if (op == FUTEX_WAKE) {
        task_wakeup(chan);
        return 0;
    }
    if (op != FUTEX_WAIT) return SYSERR_BADCALL;
    if (*(volatile uint32_t *)addr != (uint32_t)value) return SYSERR_AGAIN;
    task_block(chan, 100);                              /* (wakes to notice Ctrl+C) */
    check_killed();
    return 0;
}

/* ---- Ports (stage 3) ---------------------------------------------------------------- */

static int64_t sys_port(struct exc_frame *f) {
    port_space_t *s = &P()->ports;
    char service[32];
    switch (f->rax) {
        case SYS_PORT_CREATE:
            return port_create(s);
        case SYS_PORT_SEND: {
            if (f->rdx > PORT_MSG_MAX || !user_range_ok(f->rsi, f->rdx, 0)) return SYSERR_FAULT;
            void *msg = kmalloc(f->rdx ? f->rdx : 1);
            if (!msg) return SYSERR_NOMEM;
            memcpy(msg, (const void *)f->rsi, f->rdx);
            int r = port_send(s, (int)f->rdi, msg, (uint32_t)f->rdx, (int)(int64_t)f->r10);
            kfree(msg);
            check_killed();
            return r;
        }
        case SYS_PORT_RECEIVE: {
            uint64_t max = f->rdx > PORT_MSG_MAX ? PORT_MSG_MAX : f->rdx;
            if (!user_range_ok(f->rsi, max, 1) || (f->r8 && !user_range_ok(f->r8, sizeof(int), 1)))
                return SYSERR_FAULT;
            void *buf = kmalloc(max ? max : 1);
            if (!buf) return SYSERR_NOMEM;
            int reply = -1;
            int r = port_receive(s, (int)f->rdi, buf, (uint32_t)max, (uint32_t)f->r10, &reply);
            if (r > 0) memcpy((void *)f->rsi, buf, (uint64_t)r);
            kfree(buf);
            if (f->r8) *(int *)f->r8 = reply;
            check_killed();
            return r;
        }
        case SYS_PORT_REGISTER:
            if (copy_user_string(service, f->rdi, sizeof(service)) != 0) return SYSERR_FAULT;
            return port_register(s, service, (int)f->rsi);
        case SYS_PORT_LOOKUP:
            if (copy_user_string(service, f->rdi, sizeof(service)) != 0) return SYSERR_FAULT;
            return port_lookup(s, service);
        default:
            return port_close(s, (int)f->rdi);
    }
}

/* ---- Signals (stage 4) -------------------------------------------------------------- *
 * A handled signal runs when the program next returns from a system call: the
 * kernel saves its registers on its stack and starts the C library's
 * trampoline, which calls the handler and then sigreturn. */

static int64_t sys_signal(uint64_t sig, uint64_t handler, uint64_t tramp) {
    if (sig < 1 || sig >= 32 || sig == SIGKILL) return SYSERR_BADCALL;
    process_t *p = P();
    p->sig_handler[sig] = handler;
    if (tramp) p->sig_tramp = tramp;
    return 0;
}

static int64_t sys_sigsend(int64_t pid, uint64_t sig) {
    task_t *t = task_by_pid((int)pid);
    if (!t || t->state == TASK_ZOMBIE || !t->is_user) return SYSERR_SRCH;
    if (sig < 1 || sig >= 32) return SYSERR_BADCALL;
    if (!user_is_root() && t->user.uid != user_current()->uid) return SYSERR_PERM;
    process_t *q = t->process;
    if (sig != SIGKILL && q && q->sig_handler[sig]) {
        q->sig_pending |= 1u << sig;
    } else if (sig != SIGCHLD) {                         /* the default: it ends */
        t->kill_code = sig == SIGKILL ? EXIT_KILLED : sig == SIGINT ? EXIT_INTERRUPTED : 128 + (int)sig;
    }
    task_interrupt(t);
    return 0;
}

static void deliver_signals(struct exc_frame *f) {
    task_t *t = task_current();
    process_t *p = t->is_user ? t->process : 0;
    if (!p || !p->sig_pending || !p->sig_tramp) return;
    int sig = __builtin_ctz(p->sig_pending);
    p->sig_pending &= ~(1u << sig);
    uint64_t handler = p->sig_handler[sig];
    if (!handler) return;
    uint64_t sp = (f->rsp - 128 - sizeof(struct exc_frame)) & ~(uint64_t)15;
    if (!user_range_ok(sp - 8, sizeof(struct exc_frame) + 8, 1)) process_exit(EXIT_SEGFAULT);
    memcpy((void *)sp, f, sizeof(*f));
    *(uint64_t *)(sp - 8) = 0;                          /* the handler never returns there */
    f->rsp = sp - 8;
    f->rip = p->sig_tramp;
    f->rdi = (uint64_t)sig;
    f->rsi = handler;
    f->rdx = sp;
}

static void sys_sigreturn(struct exc_frame *f, uint64_t ptr) {
    if (!user_range_ok(ptr, sizeof(struct exc_frame), 0)) process_exit(EXIT_SEGFAULT);
    struct exc_frame saved;
    memcpy(&saved, (const void *)ptr, sizeof(saved));
    saved.cs = 0x2B;                                    /* never anything but user mode */
    saved.ss = 0x33;
    saved.rflags = (saved.rflags & 0xCD5) | 0x202;
    *f = saved;
}

/* ---- The device registry and login (stages 5, 6) ------------------------------------ */

static int64_t sys_registry(uint64_t index, uint64_t ptr) {
    if (!user_range_ok(ptr, sizeof(struct os_regnode), 1)) return SYSERR_FAULT;
    const reg_node_t *n = registry_node((int)index);
    if (!n) return 0;
    struct os_regnode *out = (struct os_regnode *)ptr;
    out->id = (int)index;
    out->parent = n->parent;
    kstrncpy(out->name, n->name, sizeof(out->name));
    kstrncpy(out->cls, n->cls, sizeof(out->cls));
    kstrncpy(out->info, n->info, sizeof(out->info));
    return 1;
}

static int64_t sys_auth(uint64_t name_ptr, uint64_t pass_ptr, uint64_t out_ptr) {
    char name[32], password[64];
    if (!user_is_root()) return SYSERR_PERM;
    if (copy_user_string(name, name_ptr, sizeof(name)) != 0 ||
        copy_user_string(password, pass_ptr, sizeof(password)) != 0 ||
        !user_range_ok(out_ptr, sizeof(struct os_user), 1))
        return SYSERR_FAULT;
    user_t u;
    int ok = users_check_password(name, password, &u) == 0;
    memset(password, 0, sizeof(password));
    if (!ok) {
        task_sleep(1000);                               /* slow down guessing */
        return SYSERR_AUTH;
    }
    struct os_user *out = (struct os_user *)out_ptr;
    out->uid = u.uid;
    k_snprintf(out->name, sizeof(out->name), "%s", u.name);
    k_snprintf(out->home, sizeof(out->home), "%s", u.home);
    return 0;
}
