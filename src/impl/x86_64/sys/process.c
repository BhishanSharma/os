// process.c - user programs: loading ELF files, system calls, ending programs
//
// Every program is a task (sys/task.c) with its own page tables: the lower
// half holds its code, heap and stack, and the kernel (upper half) is mapped the
// same way in every address space, so a system call can read the program's
// memory directly once the pointer is checked (user_range_ok). Several
// programs can run at once; the timer switches between them. One of them may
// be in the foreground: it gets the keyboard and Ctrl+C.
#include "sys/process.h"
#include "process_internal.h"
#include "sys/smp.h"
#include "drivers/igfx.h"
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
#include "drivers/iwlwifi.h"
#include "drivers/wifi.h"
#include "drivers/power.h"
#include "drivers/efi.h"
#include "sys/system.h"
#include "drivers/fat32.h"
#include "drivers/fatvol.h"
#include "drivers/memory.h"
#include "net/tcp.h"
#include "net/net.h"
#include "net/net_internal.h"
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
#define USER_MMAP_LOW    0x100000000ULL     /* mmapat: never below 4 GiB (the program is there) */
#define USER_HEAP_MAX    (64ULL << 30)     /* in practice: as much as RAM gives */
#define MAX_ARGS         64   /* argv and environment strings together */
#define PAGE_UP(x)       (((x) + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1))
#define PAGE_DOWN(x)     ((x) & ~(uint64_t)(PAGE_SIZE - 1))

_Static_assert(KEYCODE_SUPER == KEY_SUPER && KEYMOD_SUPER == (int)0x4000 && KEYMOD_CTRL == 0x2000, "the Windows key");
_Static_assert(KEYCODE_HOME == KEY_HOME && KEYCODE_DELETE == KEY_DELETE && KEYCODE_INSERT == KEY_INSERT &&
               KEYCODE_PGUP == KEY_PAGE_UP && KEYCODE_PGDN == KEY_PAGE_DOWN && KEYCODE_END == KEY_END,
               "the navigation key codes");
_Static_assert(KEYCODE_UP == KEY_UP_ARROW && KEYCODE_DOWN == KEY_DOWN_ARROW &&
               KEYCODE_LEFT == KEY_LEFT_ARROW && KEYCODE_RIGHT == KEY_RIGHT_ARROW,
               "user key codes must match the keyboard driver");

static file_t console_file = { .used = 1, .refs = 1 << 30, .kind = F_CONSOLE };

/* Files bigger than this are not read into memory at open() (a video). */
#define STREAM_MIN       (1024 * 1024)


static int graphics_owner;            // pid drawing pixels (gfx), 0: the console
static int sound_owner;               // pid streaming sound (sys_sound), 0: none
static void *sound_process;           // its process: whichever of its threads ends last frees it
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
#define ELF_TYPE_DYN    3     /* a shared object: the dynamic linker */
#define ELF_MACHINE_X64 62
#define PT_LOAD         1
#define PT_INTERP       3

/* Where a dynamically linked program's dynamic linker (ld.so) goes: below
 * the mappings (vm.c's MMAP_TOP), far above any program and its heap. */
#define INTERP_BASE     0x00007E0000000000ULL

/* ---- The current program -------------------------------------------- */

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
        uint64_t e = paging_get_user_entry(root, vaddr);
        if (!(e & PAGE_PRESENT)) kpanic("copy_to_space: page not mapped");   /* (never physical page 0) */
        uint64_t phys = e & ~0xFFFULL;
        memcpy((uint8_t *)page_address(phys) + page_off, s, n);
        vaddr += n;
        s += n;
        len -= n;
    }
}

/* 1 if the running program may read (or, with `write`, write) [ptr, ptr + len).
 * Pages of its regions that are not there yet are brought in (sys/vm.h), so
 * the kernel can then use the memory directly. */
int user_range_ok(uint64_t ptr, uint64_t len, int write) {
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

int process_kernel_page_fault(uint64_t addr, uint64_t err) {
    task_t *t = task_current();
    process_t *p = t && t->is_user ? t->process : 0;
    if (!p || addr < USER_BASE || addr >= USER_TOP) return -1;
    uint64_t e = paging_get_user_entry(t->root, addr);
    if ((err & 1) && (e & PAGE_PRESENT) && (e & PAGE_USER) && (!(err & 2) || (e & PAGE_RW))) {
        kprintf("SMAP: the kernel touched program memory at 0x%lx outside a system call\n", addr);
        return -1;                                    /* a kernel bug: the panic screen */
    }
    if (vm_fault(t->root, &p->vm, addr, (err & 2) != 0) == 0) return 0;
    /* Not the program's memory any more: let the call finish on a scratch
     * page, then end the program as if it had touched the address itself. */
    if (vm_scratch_page(t->root, addr) != 0) return -1;
    kprintf("%s (pid %d): the kernel found program memory at 0x%lx gone - program ended\n",
            t->name, t->pid, addr);
    if (!t->kill_code) t->kill_code = EXIT_SEGFAULT;
    return 0;
}

int process_signal_pending(void) {
    task_t *t = task_current();
    return t && t->is_user && t->process && ((process_t *)t->process)->sig_pending != 0;
}

/* Copy a NUL-terminated string from the program; 0 on success. */
int copy_user_string(char *out, uint64_t ptr, size_t max) {
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
int user_path(uint64_t ptr, char *out, int size) {
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

file_t *file_new(int kind) {
    file_t *f = kmalloc(sizeof(file_t));
    if (!f) return 0;
    memset(f, 0, sizeof(*f));
    f->used = 1;
    f->refs = 1;
    f->kind = kind;
    return f;
}

int fd_free_slot(process_t *p, int from) {
    for (int i = from; i < MAX_FDS; i++)
        if (!p->fds[i]) return i;
    return -1;
}

file_t *fd_get(uint64_t fd) {
    process_t *p = P();
    return p && fd < MAX_FDS ? p->fds[fd] : 0;
}

static int64_t close_file(file_t *f);

/* Every reference to a pipe or a local socket counts as one of its ends. */
void ends_added(file_t *f) {
    if (f->kind == F_PIPE_R || f->kind == F_PIPE_W) {
        pipe_end_added(f->pipe, f->kind == F_PIPE_W);
    } else if (f->kind == F_USOCK && f->us) {
        pipe_end_added(f->us->pipe[f->side], 0);
        pipe_end_added(f->us->pipe[!f->side], 1);
    }
}

void ends_closed(file_t *f) {
    if (f->kind == F_PIPE_R || f->kind == F_PIPE_W) {
        pipe_end_closed(f->pipe, f->kind == F_PIPE_W);
    } else if (f->kind == F_USOCK && f->us) {
        pipe_end_closed(f->us->pipe[f->side], 0);
        pipe_end_closed(f->us->pipe[!f->side], 1);
    }
}

/* One reference less; the last one closes it. */
int64_t file_put(file_t *f) {
    if (!f || f == &console_file) return 0;
    if (--f->refs > 0) {
        ends_closed(f);
        return 0;
    }
    return close_file(f);
}

file_t *file_get(file_t *f) {
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
    } else if (f->kind == F_USOCK) {
        usock_closed(f);
    } else if (f->kind == F_ULISTEN) {
        listener_closed(f);
    } else if (f->kind == F_SHM) {
        vm_object_put(f->shm);
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

int64_t sys_close(uint64_t fd) {
    file_t *f = fd_get(fd);
    if (!f) return SYSERR_BADFD;
    P()->fds[fd] = 0;
    P()->cloexec[fd] = 0;
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
    p->cloexec[to] = 0;
    ends_added(f);
    return (int64_t)to;
}

static int64_t sys_dup(uint64_t fd) {
    if (!fd_get(fd)) return SYSERR_BADFD;
    int to = fd_free_slot(P(), 0);
    return to < 0 ? SYSERR_MFILE : sys_dup2(fd, (uint64_t)to);
}

/* ---- Pipes and descriptors (local sockets: syscall_usock.c) -------------- */

/* Give this program a descriptor for f (or drop it). */
int64_t install(file_t *f) {
    int fd = fd_free_slot(P(), 0);
    if (fd < 0) {
        file_put(f);
        return SYSERR_MFILE;
    }
    P()->fds[fd] = f;
    return fd;
}

/* A pipe read or write; non-blocking files get SYSERR_WOULDBLOCK instead of
 * waiting (and a write only what fits). */
static int64_t stream_read(file_t *f, pipe_t *pp, char *dst, uint64_t len) {
    if (f->nonblock) {
        uint32_t waiting, room;
        int no_writers, no_readers;
        pipe_state(pp, &waiting, &room, &no_writers, &no_readers);
        if (!waiting && !no_writers && len) return SYSERR_WOULDBLOCK;
    }
    int64_t n = pipe_read(pp, dst, len);
    if (n < 0) check_killed();
    return n < 0 ? SYSERR_TIMEOUT : n;
}

static int64_t stream_write(file_t *f, pipe_t *pp, const char *src, uint64_t len) {
    if (f->nonblock) {
        uint32_t waiting, room;
        int no_writers, no_readers;
        pipe_state(pp, &waiting, &room, &no_writers, &no_readers);
        if (!no_readers && !room && len) return SYSERR_WOULDBLOCK;
        if (len > room && !no_readers) len = room;
    }
    return pipe_write(pp, src, len);                    /* -2: nobody reads; -1: interrupted */
}

/* ---- Pseudo-terminals (USOCK_PTY; the modes: syscall_usock.c) ---- */

/* The window types (writes to the master): into what the slave reads. In
 * raw mode with ISIG, Ctrl+C is SIGINT (Ctrl+Z SIGTSTP, Ctrl+\ SIGQUIT) for
 * the foreground process group, or without one the program that set the mode;
 * with ECHO the keys come back to the window. In the cooked mode the window
 * has edited the line (and shown it); Ctrl+D there is an end of file. */
static int64_t pty_type(file_t *f, const char *src, uint64_t len) {
    pty_t *t = f->us->pty;
    pipe_t *to_slave = f->us->pipe[1], *to_window = f->us->pipe[0];
    int raw = !(t->t.lflag & PTY_ICANON);
    uint64_t from = 0;
    for (uint64_t i = 0; i <= len; i++) {
        unsigned char c = i < len ? (unsigned char)src[i] : 0;
        int sig = !(i < len && raw && (t->t.lflag & PTY_ISIG) && c) ? 0
                  : c == t->t.cc[PTY_VINTR] ? SIGINT : c == t->t.cc[PTY_VSUSP] ? SIGTSTP
                  : c == t->t.cc[PTY_VQUIT] ? SIGQUIT : 0;
        int intr = sig != 0;
        int eof = i < len && !raw && c && c == t->t.cc[PTY_VEOF];
        if (i < len && !intr && !eof) continue;
        if (i > from) {                                   /* the plain bytes before it */
            int64_t n = stream_write(f, to_slave, src + from, i - from);
            if (n < 0) return n == -2 ? SYSERR_PIPE : n == SYSERR_WOULDBLOCK ? n : SYSERR_TIMEOUT;
            if (raw && (t->t.lflag & PTY_ECHO)) pipe_write(to_window, src + from, i - from);
        }
        from = i + 1;
        if (intr && t->pgrp && pgrp_alive(t->pgrp)) signal_group(t->pgrp, (uint64_t)sig);   /* the job in front */
        else if (intr && t->fg) signal_send(t->fg, (uint64_t)sig);
        if (eof) {
            t->eof++;
            io_event();
        }
    }
    return (int64_t)len;
}

/* The slave reads: what was typed, or 0 once for each Ctrl+D. Waits on the
 * I/O counter rather than in the pipe, so a Ctrl+D wakes it too. */
static int64_t pty_read(file_t *f, char *dst, uint64_t len) {
    pty_t *t = f->us->pty;
    pipe_t *pp = f->us->pipe[1];
    for (;;) {
        uint64_t gen = io_generation();
        uint32_t waiting, room;
        int no_writers, no_readers;
        pipe_state(pp, &waiting, &room, &no_writers, &no_readers);
        if (waiting || no_writers || !len) return stream_read(f, pp, dst, len);
        if (t->eof) {
            t->eof--;
            return 0;
        }
        if (f->nonblock) return SYSERR_WOULDBLOCK;
        io_wait(gen, 100);
        check_killed();
        if (P()->sig_pending) return SYSERR_TIMEOUT;     /* interrupted (EINTR) */
    }
}

/* What poll() needs of a pipe, local socket, file or the console: SOCK_*. */
static int64_t fd_flags(file_t *fl) {
    int64_t r;
    if (fl->kind == F_PIPE_R || fl->kind == F_PIPE_W) {
        uint32_t waiting, room;
        int no_writers, no_readers;
        pipe_state(fl->pipe, &waiting, &room, &no_writers, &no_readers);
        if (fl->kind == F_PIPE_R)
            r = (waiting || no_writers ? SOCK_READABLE : 0) | (no_writers ? SOCK_HANGUP : 0);
        else
            r = (room || no_readers ? SOCK_WRITABLE : 0) | (no_readers ? SOCK_HANGUP : 0);
    } else if (fl->kind == F_USOCK) {
        r = fl->us && fl->us->pty ? SOCK_PTY : SOCK_LOCAL;
        if (!fl->us) return r | SOCK_HANGUP;
        if (fl->us->pty && fl->side == 1 && fl->us->pty->eof) r |= SOCK_READABLE;
        uint32_t waiting, room;
        int no_writers, no_readers;
        pipe_state(fl->us->pipe[fl->side], &waiting, &room, &no_writers, &no_readers);
        if (waiting || no_writers || fl->us->fdq_n[fl->side]) r |= SOCK_READABLE;
        if (no_writers) r |= SOCK_HANGUP;
        pipe_state(fl->us->pipe[!fl->side], &waiting, &room, &no_writers, &no_readers);
        if (room || no_readers) r |= SOCK_WRITABLE;
    } else if (fl->kind == F_SHM) {
        r = SOCK_READABLE | SOCK_WRITABLE | SOCK_SHM;
    } else if (fl->kind == F_ULISTEN) {
        r = SOCK_LOCAL | SOCK_LISTENING | (fl->ul->n ? SOCK_READABLE : 0);
    } else if (fl->kind == F_CONSOLE) {
        r = SOCK_WRITABLE;                                /* (the keyboard: read() waits) */
    } else {
        r = SOCK_READABLE | SOCK_WRITABLE;                /* files never wait */
    }
    return r;
}

/* poll() of the kernel's descriptors in one call (SYS_POLL): a browser's
 * event loop asked fdinfo of each, then pollwait, every time round. */
static int64_t sys_poll(uint64_t ptr, uint64_t n, int64_t ms) {
    if (n > POLL_MAX || !user_range_ok(ptr, n * sizeof(struct os_pollfd), 1)) return SYSERR_FAULT;
    struct os_pollfd *fds = (struct os_pollfd *)ptr;
    uint32_t start = timer_ms();
    for (;;) {
        uint64_t gen = io_generation();
        int ready = 0;
        for (uint64_t i = 0; i < n; i++) {
            int fd = fds[i].fd;
            file_t *fl = fd >= 0 ? fd_get((uint64_t)fd) : 0;
            int64_t got = fd < 0 ? 0 : fl ? fd_flags(fl) : -1;
            fds[i].got = (int)got;
            if (got < 0 || (got & (fds[i].events | SOCK_HANGUP))) ready++;
        }
        uint32_t spent = timer_ms() - start;
        if (ready || ms == 0 || (ms > 0 && spent >= (uint64_t)ms)) return ready;
        io_wait(gen, ms < 0 ? NO_LIMIT_MS : (uint32_t)(ms - spent));
        check_killed();
        if (P()->sig_pending) return 0;                 /* (the handler runs; the caller asks again) */
    }
}

/* poll()'s wait: until a pipe or local socket changes, ms pass, or (net) the
 * next interrupt, since a packet may have come. */
static int64_t sys_pollwait(int64_t gen, uint64_t ms, uint64_t net) {
    if (gen < 0) return (int64_t)io_generation();
    if (ms > NO_LIMIT_MS) ms = NO_LIMIT_MS;
    if ((uint64_t)gen == io_generation() && ms) {
        if (net) {
            tcp_tick();
            net_wait();
        } else {
            io_wait((uint64_t)gen, (uint32_t)ms);
        }
    }
    return (int64_t)io_generation();
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
    if (sound_owner == t->pid || (p && (void *)p == sound_process)) {   /* and its sound */
        sound_stream_close();
        sound_owner = 0;
        sound_process = 0;
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
    if (graphics_owner == t->pid) {                    // the screen (before its memory goes: the GPU may read it)
        graphics_owner = 0;
        fbcon_cursor_set(0, 0, 0, 0, 0, 0, 0);
        print_set_graphics(0);
    }
    t->root = paging_kernel_root();
    paging_switch(t->root);
    paging_free_address_space(root);
    if (mouse_owner() == t->pid) mouse_set_owner(0);  // the console gets the mouse back
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
void check_killed(void) {
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

void process_fault(uint64_t vector, const char *name, uint64_t rip, uint64_t address, uint64_t rsp) {
    task_t *t = task_current();
    if (!t || !t->is_user) return;
    print_set_theme_colors();
    if (print_get_col() != 0) print_str("\n");
    char msg[200];
    process_t *p = t->process;
    int overflow = p && address < p->stack_lo && address + (1u << 20) >= p->stack_lo;
    if (vector == 14 && vm_out_of_memory)
        k_snprintf(msg, sizeof(msg), "%s (pid %d): out of memory (the computer's memory is full) - program terminated",
                   t->name, t->pid);
    else if (vector == 14)
        k_snprintf(msg, sizeof(msg), "%s (pid %d): %s at address 0x%lx (rip 0x%lx) - program terminated",
                   t->name, t->pid,
                   overflow ? "Stack overflow (more than 8 MiB of stack)"
                   : address < USER_BASE || address >= USER_TOP ? "Segmentation fault (kernel or unmapped memory)"
                                                                : "Segmentation fault",
                   address, rip);
    else
        k_snprintf(msg, sizeof(msg), "%s (pid %d): %s (rip 0x%lx) - program terminated", t->name, t->pid, name, rip);
    print_error(msg);
    /* A jump to nowhere (a null function pointer, or a return address the
     * program overwrote): the stack around it, for its callers (nm). */
    if (rip < USER_BASE && user_range_ok(rsp - 64, 192, 0)) {
        uint64_t w[24];
        user_access_begin();
        memcpy(w, (const void *)(rsp - 64), sizeof(w));
        user_access_end();
        for (int i = 0; i < 24; i += 4) {
            k_snprintf(msg, sizeof(msg), "  stack rsp%d: %016lx %016lx %016lx %016lx", i * 8 - 64,
                       w[i], w[i + 1], w[i + 2], w[i + 3]);
            print_error(msg);
        }
    }
    process_exit(vector == 0 ? EXIT_ARITHMETIC : vector == 6 ? EXIT_ILLEGAL : EXIT_SEGFAULT);
}

/* Timer interrupt that arrived in user mode (irq.asm): stop the program if it
 * was asked to, otherwise switch if its time slice is used up. */
static void stop_point(void);
void user_check_interrupt(void) {
    task_t *t = task_current();                       /* nothing for the kernel: no lock */
    process_t *p = t && t->is_user ? t->process : 0;
    if (p && !t->kill_code && !p->stop_sig && !task_freezing() && !task_slice_over()) return;
    bkl_enter();
    check_killed();
    stop_point();
    check_killed();
    task_preempt();
    task_freeze_point();
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
    print_splash_end();                             /* a text login: show the console */
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
    if (f->kind == F_USOCK && f->us && f->us->pty && f->side == 0) return pty_type(f, src, len);
    if (f->kind == F_USOCK) {
        if (!f->us) return SYSERR_BADFD;
        int64_t n = stream_write(f, f->us->pipe[!f->side], (const char *)src, len);
        return n == -2 ? SYSERR_PIPE : n == -1 ? SYSERR_TIMEOUT : n;   /* (no SIGPIPE, like MSG_NOSIGNAL) */
    }
    if (f->kind == F_PIPE_W) {
        int64_t n = stream_write(f, f->pipe, (const char *)src, len);
        if (n == SYSERR_WOULDBLOCK) return n;
        if (n == -2) {                                  /* nobody reads: SIGPIPE ends it */
            process_t *p = P();
            if (!p->sig_handler[SIGPIPE]) process_exit(128 + SIGPIPE);
            p->sig_pending |= 1u << SIGPIPE;
            return SYSERR_PIPE;
        }
        return n < 0 ? SYSERR_TIMEOUT : n;
    }
    if (f->kind == F_SHM) {                             /* into the memory, at the position */
        int64_t n = vm_shm_copy(f->shm, f->pos, (void *)src, len, 1);
        if (n < 0) return SYSERR_NOMEM;
        f->pos += (uint32_t)n;
        return n;
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
    if (f->kind == F_PIPE_R) return stream_read(f, f->pipe, dst, len);
    if (f->kind == F_USOCK && f->us && f->us->pty && f->side == 1) return pty_read(f, dst, len);
    if (f->kind == F_USOCK) return f->us ? stream_read(f, f->us->pipe[f->side], dst, len) : SYSERR_BADFD;
    if (f->kind == F_SHM) {
        int64_t n = vm_shm_copy(f->shm, f->pos, dst, len, 0);
        if (n < 0) return SYSERR_NOMEM;
        f->pos += (uint32_t)n;
        return n;
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
    if (f && f->kind == F_SHM) {                        /* (the end is the memory's size) */
        int64_t end = (int64_t)vm_object_size(f->shm);
        int64_t at = (whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f->pos : end) + offset;
        if (whence > SEEK_END || at < 0) return SYSERR_BADCALL;
        f->pos = (uint32_t)(at < end ? at : end);
        return f->pos;
    }
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
    task_t *t = task_current();                     /* the program's: any of its threads may use it */
    for (int hops = 0; t->is_thread && hops < 8; hops++) {
        task_t *p = task_by_pid(t->parent);
        if (!p) break;
        t = p;
    }
    int pid = t->pid;
    if (op == SOUND_OPEN) {
        if (sound_owner && sound_owner != pid) return SYSERR_INUSE;
        if (sound_owner == pid) return 0;
        int r = sound_stream_open();
        if (r == -2) return SYSERR_INUSE;
        if (r != 0) return SYSERR_NOENT;
        sound_owner = pid;
        sound_process = task_current()->process;
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
        sound_process = 0;
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
    uint32_t end = timer_ms() + (uint32_t)ms;
    for (int32_t left; (left = (int32_t)(end - timer_ms())) > 0;) {
        check_killed();
        task_sleep(left > 50 ? 50 : (uint32_t)left);
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
        if (print_graphics_mode() < 2 && scroll_key(c)) continue;   /* the desktop: they are its keys */
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
        case VFS_ERR_NO_OWNERS:    return SYSERR_NOTSUP;
        case VFS_ERR_CROSS:        return SYSERR_XDEV;
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
    vfs_stat_t st;
    int r = vfs_stat(path, &st);
    if (r) return fs_result(r);
    out->is_dir = (unsigned char)st.is_dir;
    out->size = st.size;
    return 0;
}

static int64_t sys_statx(uint64_t path_ptr, uint64_t ptr) {
    char path[MAX_PATH];
    if (user_path(path_ptr, path, sizeof(path)) != 0) return SYSERR_FAULT;
    if (!user_range_ok(ptr, sizeof(struct os_statx), 1)) return SYSERR_FAULT;
    vfs_stat_t st;
    int r = vfs_stat(path, &st);
    if (r) return fs_result(r);
    struct os_statx *out = (struct os_statx *)ptr;
    out->size = st.size;
    out->is_dir = (unsigned char)st.is_dir;
    out->owners = (unsigned char)st.owners;
    out->mode = st.mode;
    out->uid = st.uid;
    out->gid = st.gid;
    out->mtime = st.mtime;
    return 0;
}

static int64_t sys_chmod(uint64_t path_ptr, uint64_t mode) {
    char path[MAX_PATH];
    if (user_path(path_ptr, path, sizeof(path)) != 0) return SYSERR_FAULT;
    return fs_result(vfs_chmod(path, (uint32_t)mode & 07777));
}

static int64_t sys_chown(uint64_t path_ptr, uint64_t uid, uint64_t gid) {
    char path[MAX_PATH];
    if (user_path(path_ptr, path, sizeof(path)) != 0) return SYSERR_FAULT;
    return fs_result(vfs_chown(path, (int64_t)(int32_t)uid, (int64_t)(int32_t)gid));
}

/* ---- Sandbox ------------------------------------------------------------- */

static int under(const char *path, const char *dir) {
    size_t n = strlen(dir);
    for (size_t i = 0; i < n; i++) {
        char a = path[i], b = dir[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return path[n] == '/' || path[n] == 0;
}

/* For the VFS: a sandboxed program keeps out of the homes and the USB stick
 * except for its folders (the rest is the system's, guarded by owners). */
int process_sandbox_allows(const char *abs) {
    task_t *t = task_current();
    if (!t || !t->is_user || !P()->sandbox.on) return 1;
    if (!under(abs, "/HOME") && !under(abs, "/USB")) return 1;
    for (int i = 0; i < SANDBOX_DIRS; i++)
        if (P()->sandbox.dirs[i][0] && under(abs, P()->sandbox.dirs[i])) return 1;
    return 0;
}

int process_may_network(void) {
    task_t *t = task_current();
    return !t || !t->is_user || !P()->sandbox.on || P()->sandbox.net;
}

static int64_t sys_sandbox(uint64_t op, uint64_t dirs_ptr, uint64_t net) {
    char dirs[SANDBOX_DIRS * 96];
    if (op > SANDBOX_SELF) return SYSERR_BADCALL;
    if (dirs_ptr && copy_user_string(dirs, dirs_ptr, sizeof(dirs)) != 0) return SYSERR_FAULT;
    if (!dirs_ptr) dirs[0] = 0;
    process_t *p = P();
    if (op == SANDBOX_SELF && p->sandbox.on) return SYSERR_PERM;        /* no way out, nor wider */
    sandbox_t box;
    memset(&box, 0, sizeof(box));
    box.on = 1;
    box.net = net != 0 && !(p->sandbox.on && !p->sandbox.net);
    int n = 0;
    for (char *line = dirs, *next; line && *line && n < SANDBOX_DIRS; line = next) {
        next = 0;
        for (char *c = line; *c; c++)
            if (*c == '\n') {
                *c = 0;
                next = c + 1;
                break;
            }
        char abs[MAX_PATH];
        if (!*line || vfs_absolute(user_cwd(), line, abs, sizeof(abs)) != 0 || strlen(abs) >= 96) continue;
        if (p->sandbox.on && !process_sandbox_allows(abs)) continue;            /* (inside its own box only) */
        kstrncpy(box.dirs[n++], abs, 96);
    }
    if (op == SANDBOX_SELF) p->sandbox = box;
    else p->sandbox_next = box;
    return 0;
}

static int64_t sys_rename(uint64_t from_ptr, uint64_t to_ptr) {
    char from[MAX_PATH], to[MAX_PATH];
    if (user_path(from_ptr, from, sizeof(from)) != 0 || user_path(to_ptr, to, sizeof(to)) != 0) return SYSERR_FAULT;
    return fs_result(vfs_rename(from, to));
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
static int64_t sys_exec(struct exc_frame *f, uint64_t path_ptr, uint64_t argv_ptr, uint64_t envp_ptr);
static int64_t sys_thread(uint64_t entry, uint64_t arg, uint64_t stack);
static int64_t sys_futex(uint64_t op, uint64_t addr, uint64_t value, uint64_t ms);
static int64_t sys_mmap(uint64_t len, uint64_t prot, int64_t fd);
static int64_t sys_mmap2(uint64_t len, uint64_t prot, int64_t fd, uint64_t offset);
static int64_t sys_mmapat(uint64_t addr, uint64_t len, uint64_t prot, int64_t fd);
static int64_t sys_shmfd(uint64_t op, uint64_t a, uint64_t b);
static int64_t sys_shmmap(uint64_t name_ptr, uint64_t size);
static int64_t sys_port(struct exc_frame *f);
static int64_t sys_signal(uint64_t sig, uint64_t handler, uint64_t tramp);
static int64_t sys_sigsend(int64_t pid, uint64_t sig);
static int64_t sys_job(uint64_t op, int64_t a, int64_t b);
static int process_sid(task_t *t);
static void sys_sigreturn(struct exc_frame *f, uint64_t ptr);
static void deliver_signals(struct exc_frame *f);
static int64_t sys_registry(uint64_t index, uint64_t ptr);
static int64_t sys_auth(uint64_t name_ptr, uint64_t pass_ptr, uint64_t out_ptr);
static int64_t sys_system(uint64_t op, uint64_t a, uint64_t b);

/* int 0x80 (usermode.asm). Runs with interrupts on, so a blocking call
 * (keyboard, sleep) still gets timer and keyboard interrupts. */
static void syscall_dispatch_locked(struct exc_frame *f);

/* Every system call runs under the big kernel lock (sys/smp.h), except the
 * ones that only read a counter or the caller's own task: the clock (every
 * clock_gettime of the C library; a browser's event loop asks it all the
 * time) and the process id. They run on any core at once. */
void syscall_dispatch(struct exc_frame *f) {
    if (f->rax == SYS_UPTIME) {
        f->rax = timer_ms();
        return;
    }
    if (f->rax == SYS_GETPID) {
        f->rax = (uint64_t)task_current()->pid;
        return;
    }
    task_t *self = task_current();
    self->sysno = (int)f->rax + 1;                    /* (bklstat: who holds the lock for what) */
    bkl_enter();
    user_access_begin();                              /* SMAP: the call may use the program's memory */
    syscall_dispatch_locked(f);
    stop_point();                                     /* stopped (Ctrl+Z): wait here for SIGCONT */
    task_freeze_point();                              /* the computer sleeps: wait here */
    user_access_end();
    self->sysno = 0;
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
        case SYS_UPTIME:  r = (int64_t)timer_ms(); break;
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
        case SYS_RESOLVE: case SYS_CONNECT: case SYS_LISTEN: case SYS_ACCEPT:
        case SYS_SEND:    case SYS_RECV:    case SYS_SOCKINFO: case SYS_SOCKCLOSE:
            r = sys_net(f->rax, f->rdi, f->rsi, f->rdx, f->r10);   /* syscall_net.c */
            break;
        case SYS_FDINFO: {                                /* what poll() needs of a pipe, file or console */
            file_t *fl = fd_get(f->rdi);
            r = fl ? fd_flags(fl) : SYSERR_BADFD;
            break;
        }
        case SYS_POLL:    r = sys_poll(f->rdi, f->rsi, (int64_t)f->rdx); break;
        case SYS_SEEK:    r = sys_seek(f->rdi, (int64_t)f->rsi, f->rdx); break;
        case SYS_SOUND:   r = sys_sound(f->rdi, f->rsi, f->rdx); break;
        case SYS_MKDIR:   r = sys_mkdir(f->rdi); break;
        case SYS_STAT:    r = sys_stat(f->rdi, f->rsi); break;
        case SYS_STATX:   r = sys_statx(f->rdi, f->rsi); break;
        case SYS_CHMOD:   r = sys_chmod(f->rdi, f->rsi); break;
        case SYS_CHOWN:   r = sys_chown(f->rdi, f->rsi, f->rdx); break;
        case SYS_RENAME:  r = sys_rename(f->rdi, f->rsi); break;
        case SYS_SANDBOX: r = sys_sandbox(f->rdi, f->rsi, f->rdx); break;
        case SYS_JOB:     r = sys_job(f->rdi, (int64_t)f->rsi, (int64_t)f->rdx); break;
        case SYS_LISTDIR: r = sys_listdir(f->rdi, f->rsi, f->rdx); break;
        case SYS_MEMINFO: r = sys_meminfo(f->rdi); break;
        case SYS_MMAP:    r = sys_mmap(f->rdi, f->rsi, (int64_t)f->rdx); break;
        case SYS_MUNMAP:                                  /* len 0: the whole mapping at addr */
            r = (f->rsi ? vm_unmap_range(P()->root, &P()->vm, f->rdi, f->rsi)
                        : vm_unmap(P()->root, &P()->vm, f->rdi)) == 0 ? 0 : SYSERR_BADCALL;
            break;
        case SYS_SHMMAP:  r = sys_shmmap(f->rdi, f->rsi); break;
        case SYS_MMAP2:   r = sys_mmap2(f->rdi, f->rsi, (int64_t)f->rdx, f->r10); break;
        case SYS_MMAPAT:  r = sys_mmapat(f->rdi, f->rsi, f->rdx, (int64_t)f->r10); break;
        case SYS_SHMFD:   r = sys_shmfd(f->rdi, f->rsi, f->rdx); break;
        case SYS_FORK:    r = sys_fork(f); break;
        case SYS_EXEC:    r = sys_exec(f, f->rdi, f->rsi, f->rdx); if (r == 0) return; break;
        case SYS_THREAD:  r = sys_thread(f->rdi, f->rsi, f->rdx); break;
        case SYS_THREAD_EXIT: finish((int)(int32_t)f->rdi, 0);
        case SYS_FUTEX:   r = sys_futex(f->rdi, f->rsi, f->rdx, f->r10); break;
        case SYS_PORT_CREATE: case SYS_PORT_SEND: case SYS_PORT_RECEIVE:
        case SYS_PORT_REGISTER: case SYS_PORT_LOOKUP: case SYS_PORT_CLOSE:
                          r = sys_port(f); break;
        case SYS_PIPE:    r = sys_pipe(f->rdi); break;
        case SYS_DUP:     r = sys_dup(f->rdi); break;
        case SYS_USOCK:   r = sys_usock(f->rdi, f->rsi, f->rdx, f->r10); break;
        case SYS_FDFLAGS: {
            process_t *p = P();
            if (!fd_get(f->rdi)) { r = SYSERR_BADFD; break; }
            if ((int64_t)f->rsi >= 0) p->cloexec[f->rdi] = f->rsi & 1;
            r = p->cloexec[f->rdi];
            break;
        }
        case SYS_EXEPATH: {
            process_t *p = P();
            uint64_t n = strlen(p->exe);
            if (!user_range_ok(f->rdi, f->rsi, 1)) { r = SYSERR_FAULT; break; }
            if (n >= f->rsi) n = f->rsi ? f->rsi - 1 : 0;
            if (f->rsi) { memcpy((char *)f->rdi, p->exe, n); ((char *)f->rdi)[n] = 0; }
            r = (int64_t)n;
            break;
        }
        case SYS_FDPATH: {                              /* the file an open file descriptor reads */
            file_t *fl = fd_get(f->rdi);
            if (!fl || fl->kind != F_FILE) { r = SYSERR_BADFD; break; }
            uint64_t n = strlen(fl->path);
            if (!user_range_ok(f->rsi, f->rdx, 1)) { r = SYSERR_FAULT; break; }
            if (n >= f->rdx) n = f->rdx ? f->rdx - 1 : 0;
            if (f->rdx) { memcpy((char *)f->rsi, fl->path, n); ((char *)f->rsi)[n] = 0; }
            r = (int64_t)n;
            break;
        }
        case SYS_POLLWAIT: r = sys_pollwait((int64_t)f->rdi, f->rsi, f->rdx); break;
        case SYS_DUP2:    r = sys_dup2(f->rdi, f->rsi); break;
        case SYS_SIGNAL:  r = sys_signal(f->rdi, f->rsi, f->rdx); break;
        case SYS_SIGSEND: r = sys_sigsend((int64_t)f->rdi, f->rsi); break;
        case SYS_SIGRETURN: sys_sigreturn(f, f->rdi); deliver_signals(f); return;
        case SYS_REGISTRY: r = sys_registry(f->rdi, f->rsi); break;
        case SYS_AUTH:    r = sys_auth(f->rdi, f->rsi, f->rdx); break;
        case SYS_SYSTEM:  r = sys_system(f->rdi, f->rsi, f->rdx); break;
        case SYS_TCB_SET:
            if (f->rdi >= USER_TOP) { r = SYSERR_FAULT; break; }
            task_current()->fs_base = f->rdi;
            __asm__ volatile("wrmsr" : : "c"(0xC0000100u), "a"((uint32_t)f->rdi), "d"((uint32_t)(f->rdi >> 32)));
            r = 0;
            break;
        default:          r = SYSERR_BADCALL; break;
    }
    f->rax = (uint64_t)r;
    check_killed();                                   /* (e.g. its buffer vanished meanwhile) */
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
/* The last load_elf: where its program headers are (C libraries find the
 * thread-local storage template through them: AT_PHDR). */
static uint64_t phdr_at, phdr_num;

/* A program is read whole into memory, unless it is bigger than BIG_PROGRAM:
 * then only its first PROGRAM_HEAD bytes are read (the headers), and its
 * segments are mapped from the file `path`, read page by page when touched. */
#define BIG_PROGRAM  (8u << 20)
#define PROGRAM_HEAD (64u * 1024)

/* With bias 0 a program (its PT_INTERP, if any, in interp); else a shared
 * object (the dynamic linker), moved up by bias. */
static uint64_t load_elf(uint64_t root, vm_space_t *vm, const uint8_t *data, uint32_t size,
                         uint64_t *image_end, const char *path, uint64_t bias, char *interp) {
    phdr_at = phdr_num = 0;
    if (interp) interp[0] = 0;
    uint32_t have = path && size > PROGRAM_HEAD ? PROGRAM_HEAD : size;   /* bytes in data */
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)data;
    if (have < sizeof(*eh) || eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' ||
        eh->ident[3] != 'F' || eh->ident[4] != 2 /* 64-bit */ || eh->ident[5] != 1 /* little endian */ ||
        eh->type != (bias ? ELF_TYPE_DYN : ELF_TYPE_EXEC) || eh->machine != ELF_MACHINE_X64 ||
        eh->phentsize != sizeof(elf64_phdr_t) || eh->phoff > have ||
        (uint64_t)eh->phnum * sizeof(elf64_phdr_t) > have - eh->phoff)
        return 0;
    vm_object_t *file = path ? vm_file_object(path) : 0;
    if (path && !file) return 0;

    uint64_t limit = USER_TOP - USER_STACK_SIZE - USER_HEAP_MAX;
    uint64_t lo = USER_TOP, hi = USER_BASE;
    for (int i = 0; i < eh->phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(data + eh->phoff + i * sizeof(elf64_phdr_t));
        if (ph->type == PT_INTERP && interp) {         /* "/lib/ld.so": dynamically linked */
            if (ph->offset > have || ph->filesz < 2 || ph->filesz > MAX_PATH || ph->filesz > have - ph->offset ||
                data[ph->offset + ph->filesz - 1] != 0)
                return 0;
            memcpy(interp, data + ph->offset, ph->filesz);
        }
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        uint64_t vaddr = ph->vaddr + bias;
        /* (written so that nothing can wrap around: the file is the program's to craft) */
        if (ph->vaddr > limit || vaddr < USER_BASE || vaddr >= limit || ph->memsz > limit - vaddr ||
            ph->filesz > ph->memsz || ph->offset > size || ph->filesz > size - ph->offset)
            return 0;
        if (file) {
            uint64_t start = PAGE_DOWN(vaddr), fend = PAGE_UP(vaddr + ph->filesz);
            uint64_t mend = PAGE_UP(vaddr + ph->memsz);
            int prot = VM_READ | ((ph->flags & 2) ? VM_WRITE : 0);   /* PF_W */
            if (vaddr - start != (ph->offset & (PAGE_SIZE - 1)) || start < hi) {
                vm_object_put(file);                   /* (not page aligned, or segments share a page) */
                return 0;
            }
            if (fend > start) {
                vm_region_t *r = vm_add(vm, start, fend, prot, VM_FILE, file);
                if (!r) { vm_object_put(file); return 0; }
                vm_object_get(file);
                r->offset = ph->offset - (vaddr - start);
                r->limit = ph->offset + ph->filesz;
            }
            if (mend > fend && !vm_add(vm, fend > start ? fend : start, mend, prot, VM_ANON, 0)) {
                vm_object_put(file);
                return 0;
            }
        } else {
            if (map_user_range(root, vaddr, vaddr + ph->memsz) != 0) return 0;
            copy_to_space(root, vaddr, data + ph->offset, ph->filesz);
        }
        if (PAGE_DOWN(vaddr) < lo) lo = PAGE_DOWN(vaddr);
        if (PAGE_UP(vaddr + ph->memsz) > hi) hi = PAGE_UP(vaddr + ph->memsz);
        if (ph->offset == 0) phdr_at = vaddr + eh->phoff;          /* the headers are loaded with it */
    }
    if (file) vm_object_put(file);                     /* (the regions hold theirs) */
    if (hi <= lo || eh->entry + bias < lo || eh->entry + bias >= hi) return 0;
    *image_end = hi;
    phdr_num = eh->phnum;
    return eh->entry + bias;
}

/* The program, and for a dynamically linked one its dynamic linker: the
 * file PT_INTERP names, from the system folder only (the RAM disk's
 * /SYS/LIB/ld.so on an installed system), never from a user's files.
 * Returns where to start (ld.so's entry, or the program's); the program's
 * own entry goes in *program_entry (AT_ENTRY), ld.so's base in interp_base
 * (AT_BASE, 0 without one). */
static uint64_t interp_base;

static uint64_t load_program(uint64_t root, vm_space_t *vm, const uint8_t *data, uint32_t size,
                             uint64_t *image_end, const char *path, uint64_t *program_entry) {
    char interp[MAX_PATH], file[MAX_PATH + 8];
    interp_base = 0;
    uint64_t entry = load_elf(root, vm, data, size, image_end, path, 0, interp);
    *program_entry = entry;
    if (!entry || !interp[0]) return entry;
    if (interp[0] != '/') return 0;
    k_snprintf(file, sizeof(file), "%s%s", strcmp(vfs_system_dir(), "/") ? vfs_system_dir() : "", interp);
    uint32_t n = fat32_get_file_size(file);
    if (n == 0 || n == 0xFFFFFFFF || n > BIG_PROGRAM) return 0;
    uint8_t *image = kmalloc(n);
    if (!image) return 0;
    uint64_t at = phdr_at, num = phdr_num, end;
    uint64_t start = fat32_read_file(file, image, n) < 0 ? 0 : load_elf(root, vm, image, n, &end, 0, INTERP_BASE, 0);
    kfree(image);
    phdr_at = at;                                      /* (AT_PHDR: the program's, not ld.so's) */
    phdr_num = num;
    if (start) interp_base = INTERP_BASE;
    return start;
}

/* argc, argv[], NULL and the strings at the top of the stack, as the SysV ABI
 * has it for _start. Built in a kernel copy of the top page, then copied in.
 * Returns the initial stack pointer, or 0 if the arguments do not fit. */
/* The new program's stack, as Linux lays it out: argc, argv..NULL, the
 * environment..NULL, the auxiliary vector; the strings above them, all in the
 * top page. args holds argc arguments and then envc environment strings. */
static uint64_t build_stack(uint64_t root, int argc, int envc, char **args, uint64_t entry_at) {
    static uint8_t page[PAGE_SIZE];
    uint64_t base = USER_TOP - PAGE_SIZE;
    uint64_t sp = USER_TOP, ptrs[MAX_ARGS];
    int n = argc + envc;
    sp -= 16;                                   /* AT_RANDOM's 16 bytes */
    uint64_t random = sp;
    for (int i = 0; i < 16; i++) page[sp - base + i] = (uint8_t)(timer_ms() * 2654435761u >> (i % 4 * 8)) ^ (uint8_t)i;
    for (int i = n - 1; i >= 0; i--) {
        size_t len = strlen(args[i]) + 1;
        if (sp - len < base + 1024) return 0;
        sp -= len;
        memcpy(page + (sp - base), args[i], len);
        ptrs[i] = sp;
    }
    sp &= ~(uint64_t)15;
    /* The auxiliary vector: AT_PHDR 3, AT_PHENT 4, AT_PHNUM 5, AT_PAGESZ 6,
     * AT_ENTRY 9, AT_RANDOM 25, AT_BASE 7, AT_NULL. */
    uint64_t aux[] = { 3, phdr_at, 4, 56, 5, phdr_num, 6, PAGE_SIZE, 9, entry_at, 25, random,
                       7, interp_base, 0, 0 };     /* AT_BASE 7: where ld.so is (0: none) */
    int words = n + 2 + 1 + (int)(sizeof(aux) / 8);   /* argc, argv..NULL, envp..NULL, auxv */
    if (words % 2) sp -= 8;               // keep the final rsp 16-byte aligned
    for (int i = (int)(sizeof(aux) / 8) - 1; i >= 0; i--) {
        sp -= 8;
        *(uint64_t *)(page + (sp - base)) = aux[i];
    }
    sp -= 8;
    *(uint64_t *)(page + (sp - base)) = 0;   /* the environment's NULL */
    for (int i = n - 1; i >= argc; i--) {
        sp -= 8;
        *(uint64_t *)(page + (sp - base)) = ptrs[i];
    }
    sp -= 8;
    *(uint64_t *)(page + (sp - base)) = 0;   /* argv's NULL */
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
static char found_program[MAX_PATH];     /* (below) */

static int spawn_as(const uint8_t *data, uint32_t size, const char *path, int argc, char **argv,
                    const user_t *user, const char *cwd, process_t *parent) {
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
    uint64_t image_end = 0, program_entry = 0, sp = 0;
    uint64_t entry = load_program(root, &p->vm, data, size, &image_end, path, &program_entry);
    int err = entry ? 0 : PROC_ERR_NOT_ELF;
    p->stack_lo = USER_TOP - USER_STACK_SIZE;
    if (!err && (!vm_add(&p->vm, p->stack_lo, USER_TOP, VM_READ | VM_WRITE, VM_ANON, 0) ||
                 !vm_add(&p->vm, image_end, image_end, VM_READ | VM_WRITE, VM_ANON, 0) ||
                 map_user_range(root, USER_TOP - PAGE_SIZE, USER_TOP) != 0))
        err = PROC_ERR_NO_MEMORY;
    if (!err && !(sp = build_stack(root, argc, 0, argv, program_entry))) err = PROC_ERR_NO_MEMORY;
    if (err) {
        vm_free(&p->vm);
        paging_free_address_space(root);
        kfree(p);
        return err;
    }
    p->brk_start = p->brk = image_end;
    p->root = root;
    p->threads = 1;
    kstrncpy(p->exe, path ? path : found_program, sizeof(p->exe));
    for (int i = 0; i < 3; i++) {                       /* 0-2 from whoever started it */
        file_t *f = parent && parent->fds[i] ? parent->fds[i] : &console_file;
        p->fds[i] = f;
        file_get(f);
        ends_added(f);
    }
    kstrncpy(p->cwd, cwd, sizeof(p->cwd));
    if (parent && parent->sandbox.on) {
        p->sandbox = parent->sandbox;                   /* what a sandboxed app starts stays in it */
    } else if (parent && parent->sandbox_next.on) {
        p->sandbox = parent->sandbox_next;
        parent->sandbox_next.on = 0;
    }

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
    p->pgid = p->sid = t->pid;                         /* a group and session of its own (job control) */
    __asm__ volatile("sti");
    return t->pid;
}

int process_spawn(const uint8_t *data, uint32_t size, int argc, char **argv) {
    char cwd[MAX_PATH];
    fat32_get_current_directory(cwd, sizeof(cwd));        // the kernel shell's
    return spawn_as(data, size, 0, argc, argv, user_current(), cwd, 0);
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
    /* A thread can be freed by someone else (process_reap_orphans): stop then. */
#define GONE(t) ((t)->state == TASK_FREE || (t)->pid != pid)
    while (t->state != TASK_ZOMBIE && !GONE(t)) {
        if (t->is_user && t->process && ((process_t *)t->process)->stop_sig)   /* a waiter without job control: */
            sys_sigsend(pid, SIGCONT);                          /* its child goes on */
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
    int code = 0;
    if (!GONE(t)) {
        code = t->exit_code;
        task_reap(t);
    }
#undef GONE
    if (foreground && print_get_col() != 0) print_str("\n");
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

/* Read `name` or `name.elf` from the current directory into a kmalloc'd buffer.
 * With big_path, a big program's headers only (load_elf maps the rest from
 * the file, whose absolute path goes in big_path). */
static char found_program[MAX_PATH];     /* the file read_program_file read last */

static uint8_t *read_program_file(const char *name, uint32_t *size, char *big_path) {
    char path[MAX_PATH + 8];
    k_snprintf(path, sizeof(path), "%s", name);
    if (!fat32_file_exists(path)) {
        k_snprintf(path, sizeof(path), "%s.elf", name);
        if (!fat32_file_exists(path)) return 0;
    }
    kstrncpy(found_program, path, sizeof(found_program));
    uint32_t n = fat32_get_file_size(path);
    if (n == 0 || n == 0xFFFFFFFF) return 0;
    if (big_path) big_path[0] = 0;
    if (big_path && n > BIG_PROGRAM && path[0] == '/') {
        uint8_t *head = kmalloc(PROGRAM_HEAD);
        vfs_file_t vf;
        if (!head) return 0;
        kmutex_lock(&vfs_lock);
        int r = vfs_open_file(path, &vf);
        if (r == 0) r = vfs_read_at(&vf, 0, head, PROGRAM_HEAD);
        kmutex_unlock(&vfs_lock);
        if (r < 0) {
            kfree(head);
            return 0;
        }
        kstrncpy(big_path, path, MAX_PATH);
        *size = n;
        return head;
    }
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
static uint8_t *find_program_from(const char *dir, const char *name, uint32_t *size, char *big_path) {
    char path[MAX_PATH];
    if (vfs_absolute(dir, name, path, sizeof(path)) != 0) return 0;
    uint8_t *data = read_program_file(path, size, big_path);
    int has_folder = 0;
    for (const char *c = name; *c; c++)
        if (*c == '/') has_folder = 1;
    if (data) return data;
    /* An absolute path that is not here may be one of the system's own files
     * (the RAM disk's /ladybird/bin/Ladybird): when the system folder is not
     * "/" (an installed system: files on the disk, the RAM disk at /SYS), look
     * there too. */
    if (name[0] == '/' && strcmp(vfs_system_dir(), "/")) {
        k_snprintf(path, sizeof(path), "%s%s", vfs_system_dir(), name);
        return read_program_file(path, size, big_path);
    }
    if (has_folder) return 0;
    /* Not here: the system folder (the RAM disk's programs). */
    k_snprintf(path, sizeof(path), "%s/%s", strcmp(vfs_system_dir(), "/") ? vfs_system_dir() : "", name);
    return read_program_file(path, size, big_path);
}

uint8_t *process_find_program(const char *name, uint32_t *size) {
    char cwd[MAX_PATH];
    fat32_get_current_directory(cwd, sizeof(cwd));
    return find_program_from(cwd, name, size, 0);
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
    char big[MAX_PATH];
    uint8_t *image = find_program_from(user_ptr ? "/" : user_cwd(), path, &size, big);
    char cwd[MAX_PATH];
    kstrncpy(cwd, user_cwd(), sizeof(cwd));
    int64_t result = image ? spawn_as(image, size, big[0] ? big : 0, argc, argv, &as, cwd, P())
                           : PROC_ERR_NOT_FOUND;
    kfree(image);
    kfree(strings);
    return result < 0 ? proc_error((int)result) : result;
}

/* wait(pid, &status, flags): a child of the caller; pid -1 = any of them. */
static int64_t sys_wait(int64_t pid, uint64_t status_ptr, uint64_t flags) {
    if (status_ptr && !user_range_ok(status_ptr, sizeof(int), 1)) return SYSERR_FAULT;
    if (pid == -1 && (flags & WAIT_FOREGROUND)) return SYSERR_BADCALL;
    task_t *me = task_current();
    if (pid == 0 || pid < -1) pid = -1;                 /* (a group of children: any child) */
    while (1) {
        task_t *child = 0, *ended = 0, *stopped = 0;
        for (int i = 0; i < MAX_TASKS; i++) {
            task_t *t = task_at(i);
            if (t->state == TASK_FREE || !t->is_user || t->parent != me->pid) continue;
            if (pid != -1 && t->pid != pid) continue;
            if (pid == -1 && t->is_thread) continue;
            child = t;
            if (t->state == TASK_ZOMBIE) { ended = t; break; }
            process_t *q = t->process;
            if (q && q->stop_sig && !q->stop_reported && (flags & WAIT_UNTRACED)) { stopped = t; break; }
            if (q && q->went_on && (flags & WAIT_CONTINUED)) {   /* went on after a stop: told once */
                q->went_on = 0;
                if (status_ptr) *(int *)status_ptr = WAIT_WENT_ON;
                return t->pid;
            }
        }
        if (!child) return SYSERR_CHILD;
        if (stopped) {                                  /* stopped (Ctrl+Z): told once */
            process_t *sq = stopped->process;
            sq->stop_reported = 1;
            if (status_ptr) *(int *)status_ptr = WAIT_STOPPED | sq->stop_sig;
            return stopped->pid;
        }
        if (pid != -1 && !(flags & (WAIT_NOHANG | WAIT_UNTRACED | WAIT_CONTINUED))) ended = child;   // wait for this one
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
/* A kernel command run by a program whose output is a pipe (the shell in a
 * Terminal window) prints into that pipe and reads its keys from the input
 * pipe, not the console under the desktop. */
static process_t *tapped(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    if (!(flags & 0x200)) return 0;                     /* an interrupt, or a lock held */
    process_t *p = task_current()->process ? P() : 0;
    return p && p->tap_out ? p : 0;
}

static void tap_flush(process_t *p) {
    if (p->tap_len) pipe_write(p->tap_out, p->tap_buf, (uint64_t)p->tap_len);
    p->tap_len = 0;
}

static int tap_print(char c) {
    process_t *p = tapped();
    if (!p) return 0;
    if (p->tap_echo && *p->tap_echo == c) {             /* the Terminal window echoes keys */
        p->tap_echo++;
        if (!*p->tap_echo) p->tap_echo = 0;
        return 1;
    }
    p->tap_echo = 0;
    p->tap_buf[p->tap_len++] = c;
    if (c == '\n' || p->tap_len == (int)sizeof(p->tap_buf)) tap_flush(p);
    return 1;
}

static int tap_key(void) {                             /* -1: not tapped; 0: no key yet */
    process_t *p = tapped();
    if (!p || !p->tap_in) return -1;
    tap_flush(p);                                      /* the prompt first */
    int c = pipe_getc(p->tap_in);
    if (c == 3) {                                      /* Ctrl+C in the window: it showed "^C" */
        (void)pipe_getc(p->tap_in);                    /* and the line end sent with it */
        p->tap_echo = "^C\n";
        return KEY_CTRL_C;
    }
    if (c == -2 || process_interrupted()) return KEY_CTRL_C;
    if (c < 0) return 0;
    if (c == '\r') c = '\n';
    static char one[2];
    one[0] = (char)c;
    p->tap_echo = c == '\b' ? "\b \b" : one;
    return c;
}

static int64_t sys_kcommand(uint64_t line_ptr) {
    char line[256], saved[256];
    if (copy_user_string(line, line_ptr, sizeof(line)) != 0) return SYSERR_FAULT;
    process_t *p = P();
    file_t *out = fd_get(1), *in = fd_get(0);
    int pty_out = out && out->kind == F_USOCK && out->us && out->us->pty && out->side == 1;
    if (out && (out->kind == F_PIPE_W || pty_out)) {
        if (!strncmp(line, "edit", 4) && (line[4] == ' ' || !line[4])) {   /* full screen: not in a pipe */
            static const char msg[] = "edit: open the file in TextEdit (textedit <file>)\n";
            pipe_write(pty_out ? out->us->pipe[0] : out->pipe, msg, sizeof(msg) - 1);
            return 1;
        }
        p->tap_out = pty_out ? out->us->pipe[0] : out->pipe;
        p->tap_in = in && in->kind == F_PIPE_R ? in->pipe
                  : in && in->kind == F_USOCK && in->us && in->us->pty && in->side == 1 ? in->us->pipe[1] : 0;
        p->tap_len = 0;
        p->tap_echo = 0;
        print_set_tap(tap_print);
        keyboard_set_tap(tap_key);
    }
    enter_cwd(saved, sizeof(saved));
    int known = shell_kernel_command(line);
    leave_cwd(saved);
    if (p->tap_out) {
        tap_flush(p);
        p->tap_out = p->tap_in = 0;
    }
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
    mouse_pixel(&m->x, &m->y);
    return 1;
}

/* gfx(op, arg): pixels for programs such as the image viewer. */
static int graphics_full;                      /* GFX_BEGIN with 1: no status bar */

/* The program drawing the screen: its own threads (the window server's painters) too. */
static int gfx_mine(int me) {
    if (graphics_owner == me) return 1;
    task_t *o = graphics_owner ? task_by_pid(graphics_owner) : 0;
    return o && o->process && o->process == task_current()->process;
}

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
            /* The foreground program, or one it just started: loginwindow hands
             * the screen to the desktop, which may ask before the hand-over
             * (wait()) has happened on another core. */
            if (foreground_pid != me && (!foreground_pid || task_current()->parent != foreground_pid))
                return SYSERR_PERM;
            if (graphics_owner && !gfx_mine(me)) return SYSERR_AGAIN;
            graphics_owner = me;
            graphics_full = arg >= 1;                        /* the desktop: the whole screen */
            print_set_graphics(graphics_full ? 2 : 1);
            if (graphics_full) top = 0;
            if (arg != 2) fbcon_fill(0, top, width, height - top, 0);   /* (2: keep the login's last picture) */
            return 0;
        case GFX_BLIT: {
            if (!gfx_mine(me)) return SYSERR_PERM;
            if (!user_range_ok(arg, sizeof(struct os_blit), 0)) return SYSERR_FAULT;
            struct os_blit b = *(const struct os_blit *)arg;
            if (b.w <= 0 || b.h <= 0 || b.stride < b.w || b.w > 16384 || b.h > 16384) return SYSERR_BADCALL;
            uint64_t bytes = ((uint64_t)(b.h - 1) * (uint64_t)b.stride + (uint64_t)b.w) * 4;
            if (!user_range_ok((uint64_t)(uintptr_t)b.pixels, bytes, 0)) return SYSERR_FAULT;
            // Keep off the status bar: skip rows above `top`.
            if (graphics_full) top = 0;
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
        case GFX_CURSOR: {                               /* the pointer, drawn by the kernel */
            if (!gfx_mine(me)) return SYSERR_PERM;
            if (!arg) return fbcon_cursor_set(0, 0, 0, 0, 0, 0, 0);
            if (!user_range_ok(arg, sizeof(struct os_cursor), 0)) return SYSERR_FAULT;
            struct os_cursor c = *(const struct os_cursor *)arg;
            if (c.w < 1 || c.h < 1 || c.w > 64 || c.h > 64) return SYSERR_BADCALL;
            if (!user_range_ok((uint64_t)(uintptr_t)c.pixels, (uint64_t)c.w * c.h * 4, 0)) return SYSERR_FAULT;
            int x, y;
            mouse_pixel(&x, &y);
            return fbcon_cursor_set(c.pixels, c.w, c.h, c.hot_x, c.hot_y, x, y) == 0 ? 0 : SYSERR_NOMEM;
        }
        case GFX_PRESENT:
            if (!gfx_mine(me)) return SYSERR_PERM;
            return fbcon_present();
        case GFX_SYNC:
            if (!gfx_mine(me)) return SYSERR_PERM;
            fbcon_sync();
            return 0;
        case GFX_DEPTH:
            return igfx_panel_bpc();
        case GFX_WAIT:
            if (!gfx_mine(me)) return SYSERR_PERM;
            fbcon_wait_shown();
            return 0;
        case GFX_GPU: {
            if (!gfx_mine(me)) return SYSERR_PERM;
            if (!igfx_gpu_ready()) return SYSERR_NOENT;
            if (!user_range_ok(arg, sizeof(struct os_gpu_op), 0)) return SYSERR_FAULT;
            struct os_gpu_op g = *(const struct os_gpu_op *)arg;
            uint64_t root = task_current()->root;
            if (g.op == GPU_MATERIAL) {
                if (g.bw < 2 || g.bh < 2 || g.bw > 1024 || g.bh > 1024 || g.w < 1 || g.w > 4096 || g.j0 < 0 ||
                    g.j1 <= g.j0 || g.j1 > 4096)
                    return SYSERR_BADCALL;
                uint64_t small = (uint64_t)g.bw * g.bh * 4;
                if (!user_range_ok((uint64_t)(uintptr_t)g.samp, small, 1) || !user_range_ok((uint64_t)(uintptr_t)g.scratch, small, 1) ||
                    !user_range_ok((uint64_t)(uintptr_t)(g.out + (uint64_t)g.j0 * g.w), (uint64_t)(g.j1 - g.j0) * g.w * 4, 1))
                    return SYSERR_FAULT;
                return igfx_gpu_material(root, g.samp, g.scratch, g.bw, g.bh, g.sat, g.tint, g.alpha, g.out, g.w,
                                         g.j0, g.j1, g.bleed) ? SYSERR_NOENT : 0;
            }
            if (g.op == GPU_OP_FADE) {
                if (g.w < 1 || g.h < 1 || g.w > 4096 || g.h > 4096 || g.pitch < g.w || g.pitch > 8192) return SYSERR_BADCALL;
                if (!user_range_ok((uint64_t)(uintptr_t)g.old, (uint64_t)g.w * g.h * 4, 0) ||
                    !user_range_ok((uint64_t)(uintptr_t)g.out, ((uint64_t)(g.h - 1) * g.pitch + g.w) * 4, 1))
                    return SYSERR_FAULT;
                return igfx_gpu_fade(root, g.old, g.out, g.w, g.h, g.pitch, g.f) ? SYSERR_NOENT : 0;
            }
            return SYSERR_BADCALL;
        }
        case GFX_GPU_DRAW: {
            if (!gfx_mine(me)) return SYSERR_PERM;
            if (!igfx_gpu_ready()) return SYSERR_NOENT;
            if (!user_range_ok(arg, sizeof(struct os_gpu_draws), 0)) return SYSERR_FAULT;
            struct os_gpu_draws d = *(const struct os_gpu_draws *)arg;
            return igfx_gpu_draw(task_current()->root, &d, user_range_ok) ? SYSERR_NOENT : 0;
        }
        case GFX_COPY: {
            if (!gfx_mine(me)) return SYSERR_PERM;
            if (!user_range_ok(arg, sizeof(struct os_copy_list), 0)) return SYSERR_FAULT;
            struct os_copy_list l = *(const struct os_copy_list *)arg;
            if (l.n < 0 || l.n > 64 || !user_range_ok((uint64_t)(uintptr_t)l.items, (uint64_t)l.n * sizeof(struct os_copy), 0))
                return SYSERR_FAULT;
            uint64_t root = task_current()->root;
            const unsigned int *last = 0;
            uint32_t va = 0;
            for (int i = 0; i < l.n; i++) {
                struct os_copy k = l.items[i];
                uint64_t bytes = (uint64_t)k.rows * (uint64_t)k.stride * 4;
                if (k.stride <= 0 || k.rows <= 0 || k.sx < 0 || k.sy < 0 || k.sx + k.w > k.stride || k.sy + k.h > k.rows ||
                    !user_range_ok((uint64_t)(uintptr_t)k.pixels, bytes, 0)) {
                    fbcon_drop_copies();
                    return SYSERR_BADCALL;
                }
                if (k.pixels != last) {                  /* (a window's rectangles come together) */
                    va = igfx_map_user(root, (uint64_t)(uintptr_t)k.pixels, bytes);
                    last = k.pixels;
                }
                if (!va || fbcon_queue_copy(k.pixels, k.stride, va, k.x, k.y, k.w, k.h, k.sx, k.sy)) {
                    fbcon_drop_copies();
                    return SYSERR_NOENT;
                }
            }
            return 0;
        }
        case GFX_SURFACE: {
            if (!gfx_mine(me)) return SYSERR_PERM;
            if (!user_range_ok(arg, sizeof(struct os_blit), 0)) return SYSERR_FAULT;
            struct os_blit b = *(const struct os_blit *)arg;
            if (b.w != (int)width || b.h != (int)height || b.stride < b.w) return SYSERR_BADCALL;
            uint64_t va = (uint64_t)(uintptr_t)b.pixels;
            uint64_t bytes = ((uint64_t)(b.h - 1) * (uint64_t)b.stride + (uint64_t)b.w) * 4;
            if (!user_range_ok(va, bytes, 0)) return SYSERR_FAULT;
            int n = (int)(((va & 0xFFF) + bytes + 4095) / 4096);
            uint64_t *pages = kmalloc((uint64_t)n * 8);
            if (!pages) return SYSERR_NOMEM;
            uint64_t root = task_current()->root;
            for (int i = 0; i < n; i++) {
                uint64_t e = paging_get_user_entry(root, (va & ~0xFFFULL) + (uint64_t)i * 4096);
                if (!(e & 1)) {                          /* not touched yet: draw once, then ask */
                    kfree(pages);
                    return SYSERR_AGAIN;
                }
                pages[i] = e & 0x000FFFFFFFFFF000ULL;
            }
            int r = fbcon_surface(b.pixels, b.stride, pages, n);
            kfree(pages);
            return r == 0 ? 0 : SYSERR_NOENT;
        }
        case GFX_END:
            if (!gfx_mine(me)) return SYSERR_PERM;
            graphics_owner = 0;
            fbcon_cursor_set(0, 0, 0, 0, 0, 0, 0);
            if (arg != 1) print_set_graphics(0);             /* (1: no console redraw: the desktop draws over it) */
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

/* mmap with an offset; a shared memory descriptor maps shared. */
static int64_t sys_mmap2(uint64_t len, uint64_t prot, int64_t fd, uint64_t offset) {
    if (offset & (PAGE_SIZE - 1)) return SYSERR_BADCALL;
    if (fd < 0) return sys_mmap(len, prot, fd);
    process_t *p = P();
    int vp = VM_READ | ((prot & PROT_WRITE) ? VM_WRITE : 0);
    file_t *f = fd_get((uint64_t)fd);
    if (!f) return SYSERR_BADFD;
    vm_object_t *o;
    int kind;
    if (f->kind == F_SHM) {
        o = f->shm;
        vm_object_get(o);
        kind = VM_SHM;
        if (!len) len = vm_object_size(o) > offset ? vm_object_size(o) - offset : 0;
    } else if (f->kind == F_FILE && f->mode == OPEN_READ) {
        if (!(o = vm_file_object(f->path))) return SYSERR_IO;
        kind = VM_FILE;
        if (!len) len = f->size > offset ? f->size - offset : 0;
    } else {
        return SYSERR_BADFD;
    }
    uint64_t a = vm_map(&p->vm, len, vp, kind, o);
    if (!a) {
        vm_object_put(o);
        return SYSERR_NOMEM;
    }
    vm_find(&p->vm, a)->offset = offset;
    return (int64_t)a;
}

/* mmapat(addr, len, prot, fd): memory at addr exactly (MAP_FIXED), in place
 * of whatever was mapped there: zeroed memory (fd -1) or shared memory from
 * its start. Programs reserve a range first and then fill parts of it. */
static int64_t sys_mmapat(uint64_t addr, uint64_t len, uint64_t prot, int64_t fd) {
    process_t *p = P();
    len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if ((addr & (PAGE_SIZE - 1)) || !len || addr < USER_MMAP_LOW || addr + len > USER_TOP - USER_STACK_SIZE ||
        addr + len < addr)
        return SYSERR_BADCALL;
    int vp = VM_READ | ((prot & PROT_WRITE) ? VM_WRITE : 0);
    vm_object_t *o = 0;
    if (fd >= 0) {
        file_t *f = fd_get((uint64_t)fd);
        if (!f || f->kind != F_SHM) return SYSERR_BADFD;
        o = f->shm;
        vm_object_get(o);
    }
    vm_unmap_range(p->root, &p->vm, addr, len);         /* (nothing there is fine) */
    if (!vm_add(&p->vm, addr, addr + len, vp, o ? VM_SHM : VM_ANON, o)) {
        if (o) vm_object_put(o);
        return SYSERR_NOMEM;
    }
    return (int64_t)addr;
}

static int64_t sys_shmfd(uint64_t op, uint64_t a, uint64_t b) {
    char name[32];
    name[0] = 0;
    if ((op == SHMFD_NEW && a) || op == SHMFD_UNLINK || op == SHMFD_EXISTS)
        if (copy_user_string(name, a, sizeof(name)) != 0) return SYSERR_FAULT;
    switch (op) {
    case SHMFD_NEW: {
        vm_object_t *o = name[0] ? vm_shm_find(name) : 0;
        if (!o && !(o = vm_shm_new(name, b))) return SYSERR_NOMEM;
        file_t *f = file_new(F_SHM);
        if (!f) {
            vm_object_put(o);
            return SYSERR_NOMEM;
        }
        f->shm = o;
        f->mode = OPEN_WRITE;
        return install(f);
    }
    case SHMFD_RESIZE: case SHMFD_SIZE: {
        file_t *f = fd_get(a);
        if (!f || f->kind != F_SHM) return SYSERR_BADFD;
        if (op == SHMFD_SIZE) return (int64_t)vm_object_size(f->shm);
        return vm_shm_resize(f->shm, b) == 0 ? 0 : SYSERR_NOMEM;
    }
    case SHMFD_UNLINK: {
        vm_object_t *o = vm_shm_find(name);
        if (!o) return SYSERR_NOENT;
        vm_shm_unlink(name);
        vm_object_put(o);
        return 0;
    }
    case SHMFD_EXISTS: {
        vm_object_t *o = vm_shm_find(name);
        vm_object_put(o);
        return o != 0;
    }
    }
    return SYSERR_BADCALL;
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
    c->sandbox_next.on = 0;
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
        ends_added(fl);
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
    t->fs_base = me->fs_base;
    c->pgid = process_pgid(me);                         /* the parent's group and session */
    c->sid = process_sid(me);
    c->stop_sig = c->stop_reported = c->went_on = 0;
    return t->pid;
}

/* Replace this program with another; its descriptors and folder stay. On
 * success the frame starts the new program (0 is returned, and not used). */
/* Copy a NULL-ended array of user strings into strings (STRINGS bytes,
 * used so far) and out[at...]: how many, or -1. */
#define STRINGS 3072
static int copy_user_strings(uint64_t array, char *strings, size_t *used, char **out, int at) {
    int n = 0;
    for (; array && at + n < MAX_ARGS; n++) {
        uint64_t slot = array + (uint64_t)n * 8;
        if (!user_range_ok(slot, 8, 0)) return -1;
        uint64_t s = *(const uint64_t *)slot;
        if (!s) break;
        if (*used >= STRINGS || copy_user_string(strings + *used, s, STRINGS - *used) != 0) return -1;
        out[at + n] = strings + *used;
        *used += strlen(out[at + n]) + 1;
    }
    return n;
}

static int64_t sys_exec(struct exc_frame *f, uint64_t path_ptr, uint64_t argv_ptr, uint64_t envp_ptr) {
    process_t *p = P();
    task_t *me = task_current();
    if (p->threads > 1) return SYSERR_AGAIN;
    char path[MAX_PATH];
    if (copy_user_string(path, path_ptr, sizeof(path)) != 0) return SYSERR_FAULT;
    char *strings = kmalloc(STRINGS), *argv[MAX_ARGS];     /* the arguments, then the environment */
    if (!strings) return SYSERR_NOMEM;
    size_t used = 0;
    int argc = copy_user_strings(argv_ptr, strings, &used, argv, 0);
    if (argc == 0) argv[argc++] = path;
    int envc = argc < 0 ? -1 : copy_user_strings(envp_ptr, strings, &used, argv, argc);
    if (argc < 0 || envc < 0) {
        kfree(strings);
        return SYSERR_FAULT;
    }
    uint32_t size = 0;
    char big[MAX_PATH];
    uint8_t *image = find_program_from(user_cwd(), path, &size, big);
    if (!image) {
        kfree(strings);
        return SYSERR_NOENT;
    }
    uint64_t root = paging_create_address_space();
    vm_space_t vm;
    vm_init_space(&vm);
    uint64_t image_end = 0, sp = 0, program_entry = 0;
    uint64_t entry = root ? load_program(root, &vm, image, size, &image_end, big[0] ? big : 0, &program_entry) : 0;
    uint64_t stack_lo = USER_TOP - USER_STACK_SIZE;
    int ok = entry && vm_add(&vm, stack_lo, USER_TOP, VM_READ | VM_WRITE, VM_ANON, 0) &&
             vm_add(&vm, image_end, image_end, VM_READ | VM_WRITE, VM_ANON, 0) &&
             map_user_range(root, USER_TOP - PAGE_SIZE, USER_TOP) == 0 &&
             (sp = build_stack(root, argc, envc, argv, program_entry)) != 0;
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
    kstrncpy(p->exe, big[0] ? big : found_program, sizeof(p->exe));
    for (int i = 0; i < MAX_FDS; i++)
        if (p->cloexec[i] && p->fds[i]) {
            file_t *cf = p->fds[i];
            p->fds[i] = 0;
            p->cloexec[i] = 0;
            file_put(cf);
        }
    p->root = me->root = root;
    p->brk_start = p->brk = image_end;
    p->stack_lo = stack_lo;
    memset(p->sig_handler, 0, sizeof(p->sig_handler));
    p->sig_pending = 0;
    me->fs_base = 0;
    __asm__ volatile("wrmsr" : : "c"(0xC0000100u), "a"(0u), "d"(0u));
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

/* FUTEX_WAIT: at most `ms` (1..100; 0 = 100) so Ctrl+C is noticed; callers loop. */
static int64_t sys_futex(uint64_t op, uint64_t addr, uint64_t value, uint64_t ms) {
    if ((addr & 3) || !user_range_ok(addr, 4, 0)) return SYSERR_FAULT;
    /* Keyed on the physical address: the same word in shared memory is the
       same futex in every program that maps it (the window server's events). */
    uint64_t *pte = paging_user_pte(P()->root, addr, 0);
    if (!pte || !(*pte & 1)) return SYSERR_FAULT;
    const void *chan = (const void *)(uintptr_t)(0x8000000000000000ULL | ((*pte & 0x000FFFFFFFFFF000ULL) + (addr & 0xFFF)));
    if (op == FUTEX_WAKE) {
        task_wakeup(chan);
        return 0;
    }
    if (op != FUTEX_WAIT) return SYSERR_BADCALL;
    if (*(volatile uint32_t *)addr != (uint32_t)value) return SYSERR_AGAIN;
    task_block(chan, ms && ms < 100 ? (uint32_t)ms : 100);   /* (wakes to notice Ctrl+C) */
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
    if (tramp && (tramp < USER_BASE || tramp >= USER_TOP)) return SYSERR_FAULT;   /* (iretq goes there) */
    process_t *p = P();
    p->sig_handler[sig] = handler;
    if (tramp) p->sig_tramp = tramp;
    return 0;
}

int64_t signal_send(int64_t pid, uint64_t sig) { return sys_sigsend(pid, sig); }

int signal_handled(int pid, int sig) {
    task_t *t = task_by_pid(pid);
    process_t *q = t && t->is_user && t->state != TASK_ZOMBIE ? t->process : 0;
    return q && sig > 0 && sig < 32 && q->sig_handler[sig] != 0;
}

/* ---- Job control: process groups, stopping and going on ---------------------------- */

int process_pgid(task_t *t) {
    process_t *p = t && t->is_user ? t->process : 0;
    return p ? (p->pgid ? p->pgid : t->pid) : 0;
}

static int process_sid(task_t *t) {
    process_t *p = t && t->is_user ? t->process : 0;
    return p ? (p->sid ? p->sid : t->pid) : 0;
}

/* A running program (not one of its threads), by pid. */
static task_t *program_by_pid(int pid) {
    task_t *t = task_by_pid(pid);
    return t && t->is_user && !t->is_thread && t->state != TASK_ZOMBIE && t->process ? t : 0;
}

static int is_program(task_t *t) {
    return t->state != TASK_FREE && t->state != TASK_ZOMBIE && t->is_user && !t->is_thread && t->process;
}

int pgrp_alive(int pgid) {
    for (int i = 0; pgid > 0 && i < MAX_TASKS; i++)
        if (is_program(task_at(i)) && process_pgid(task_at(i)) == pgid) return 1;
    return 0;
}

/* Every program of group g gets the signal -> how many did. */
int signal_group(int g, uint64_t sig) {
    int n = 0;
    for (int i = 0; g > 0 && i < MAX_TASKS; i++) {
        task_t *t = task_at(i);
        if (is_program(t) && process_pgid(t) == g && sys_sigsend(t->pid, sig) == 0) n++;
    }
    return n;
}

/* A stopped program waits here, between its system calls (and at a tick in
 * user mode), until SIGCONT; what it was waiting for in a call goes on. */
static void stop_point(void) {
    task_t *t = task_current();
    process_t *p = t->is_user ? t->process : 0;
    while (p && p->stop_sig && !t->kill_code) task_sleep(10);
}

/* The parent hears of a child's stop: SIGCHLD, if it asked for it. */
static void tell_parent(task_t *t) {
    task_t *parent = task_by_pid(t->parent);
    process_t *pp = parent && parent->is_user ? parent->process : 0;
    if (pp && pp->sig_handler[SIGCHLD]) {
        pp->sig_pending |= 1u << SIGCHLD;
        task_interrupt(parent);
    }
}

static int64_t sys_job(uint64_t op, int64_t a, int64_t b) {
    task_t *me = task_current();
    switch (op) {
    case JOB_GETPGID:
    case JOB_GETSID: {
        task_t *t = a ? program_by_pid((int)a) : me;
        if (!t || !t->process) return SYSERR_SRCH;
        return op == JOB_GETPGID ? process_pgid(t) : process_sid(t);
    }
    case JOB_SETPGID: {
        task_t *t = a ? program_by_pid((int)a) : me;
        if (!t || (t != me && t->parent != me->pid)) return SYSERR_SRCH;   /* itself or a child */
        if (b < 0) return SYSERR_BADCALL;
        int g = b ? (int)b : t->pid, sid = process_sid(me);
        if (process_pgid(t) == g) return 0;                                  /* (already) */
        if (process_sid(t) != sid || sid == t->pid) return SYSERR_PERM;     /* (a session leader stays) */
        if (g != t->pid) {                                                   /* into a group of the session */
            int found = 0;
            for (int i = 0; i < MAX_TASKS && !found; i++)
                found = is_program(task_at(i)) && process_pgid(task_at(i)) == g && process_sid(task_at(i)) == sid;
            task_t *leader = task_by_pid(g);                                 /* a pipeline's first, ended already */
            if (leader && leader->state != TASK_FREE && (leader->parent == me->pid || leader->parent == me->parent))
                found = 1;
            if (!found) return SYSERR_PERM;
        }
        ((process_t *)t->process)->pgid = g;
        return 0;
    }
    case JOB_SETSID:
        if (process_pgid(me) == me->pid) return SYSERR_PERM;                 /* a group leader cannot */
        P()->pgid = P()->sid = me->pid;
        return me->pid;
    case JOB_GETPPID:
        return me->parent;
    }
    return SYSERR_BADCALL;
}

/* pid > 0: that program; 0: the caller's group; -g: group g. SIGSTOP (and
 * SIGTSTP, SIGTTIN, SIGTTOU without a handler) stop it, SIGCONT has it go
 * on; SIGCHLD, SIGCONT and SIGWINCH are nothing to a program without a
 * handler; anything else without one ends it. */
static int64_t sys_sigsend(int64_t pid, uint64_t sig) {
    if (pid == 0 || pid < -1) {
        if (sig >= 32) return SYSERR_BADCALL;
        return signal_group(pid ? (int)-pid : process_pgid(task_current()), sig) ? 0 : SYSERR_SRCH;
    }
    task_t *t = task_by_pid((int)pid);
    if (!t || t->state == TASK_ZOMBIE || !t->is_user) return SYSERR_SRCH;
    if (sig == 0) return 0;                             /* only: is it still running? */
    if (sig < 1 || sig >= 32) return SYSERR_BADCALL;
    if (!user_is_root() && t->user.uid != user_current()->uid) return SYSERR_PERM;
    process_t *q = t->process;
    if (q && sig == SIGCONT) {
        if (q->stop_sig) q->went_on = 1;
        q->stop_sig = q->stop_reported = 0;             /* (its tasks see it within 10 ms) */
        if (!q->sig_handler[SIGCONT]) return 0;
    } else if (q && (sig == SIGSTOP || ((sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) && !q->sig_handler[sig]))) {
        if (!q->stop_sig) {
            q->stop_sig = (int)sig;
            q->stop_reported = 0;
            tell_parent(t);
        }
        return 0;
    }
    if (sig != SIGKILL && q && q->sig_handler[sig]) {
        q->sig_pending |= 1u << sig;
    } else if (sig != SIGCHLD && sig != SIGCONT && sig != SIGWINCH) {   /* the default: it ends */
        t->kill_code = sig == SIGKILL ? EXIT_KILLED : sig == SIGINT ? EXIT_INTERRUPTED : 128 + (int)sig;
    } else {
        return 0;
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
    /* iretq to a kernel or non-canonical address faults in the kernel (a panic):
     * a program that asks for one has crashed. */
    if (saved.rip < USER_BASE || saved.rip >= USER_TOP) process_exit(EXIT_SEGFAULT);
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
    if (copy_user_string(name, name_ptr, sizeof(name)) != 0 ||
        copy_user_string(password, pass_ptr, sizeof(password)) != 0 ||
        !user_range_ok(out_ptr, sizeof(struct os_user), 1))
        return SYSERR_FAULT;
    /* Anyone may check their own password (the lock screen); others' only root.
     * Not an app in a sandbox: it would only guess. */
    if (P()->sandbox.on) return SYSERR_PERM;
    if (!user_is_root() && strcmp(name, user_current()->name) != 0) return SYSERR_PERM;
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

/* ---- system(): the desktop's Wi-Fi, sound and power menus ----------------------- */

static int64_t sys_system(uint64_t op, uint64_t a, uint64_t b) {
    switch (op) {
        case SYSTEM_WIFI_STATUS: {
            if (!user_range_ok(a, sizeof(struct os_wifi), 1)) return SYSERR_FAULT;
            if (wifi_count() == 0) return SYSERR_NOENT;
            struct os_wifi *w = (struct os_wifi *)a;
            memset(w, 0, sizeof(*w));
            if (!iwl_connected(w->name, sizeof(w->name), &w->signal)) return 0;
            w->secured = 1;
            net_fmt_ip(w->address, net_get_config()->ip);
            return 1;
        }
        case SYSTEM_WIFI_SCAN: {
            int max = (int)b;
            if (max < 1 || max > 64 || !user_range_ok(a, (uint64_t)max * sizeof(struct os_wifi), 1)) return SYSERR_FAULT;
            if (wifi_count() == 0) return SYSERR_NOENT;
            if (iwl_scan() != 0) return SYSERR_IO;
            struct os_wifi *list = (struct os_wifi *)a;
            int n = 0;
            for (; n < max; n++) {
                memset(&list[n], 0, sizeof(list[n]));
                if (iwl_scan_result(n, list[n].name, sizeof(list[n].name), &list[n].signal, &list[n].secured) != 0) break;
            }
            return n;
        }
        case SYSTEM_WIFI_CONNECT: {
            char name[33], password[64];
            if (copy_user_string(name, a, sizeof(name)) != 0 || copy_user_string(password, b, sizeof(password)) != 0)
                return SYSERR_FAULT;
            int r = iwl_connect(name, password[0] ? password : 0);
            memset(password, 0, sizeof(password));
            if (r == 0) net_configure();                       /* DHCP on the new network */
            return r == 0 ? 0 : SYSERR_IO;
        }
        case SYSTEM_WIFI_OFF:
            iwl_disconnect();
            return 0;
        case SYSTEM_VOLUME:
            if (!sound_ready()) return SYSERR_NOENT;
            if ((int64_t)a >= 0) sound_set_volume((int)(a > 100 ? 100 : a));
            return sound_get_volume();
        case SYSTEM_BATTERY: {
            int charging = 0, p = power_battery(&charging);
            return p < 0 ? SYSERR_NOENT : p | (charging ? 0x100 : 0);
        }
        case SYSTEM_SETUP: {
            if (!a) return users_setup_needed();
            char name[32], password[64];
            if (copy_user_string(name, a, sizeof(name)) != 0 || copy_user_string(password, b, sizeof(password)) != 0)
                return SYSERR_FAULT;
            int r = users_setup(name, password);
            memset(password, 0, sizeof(password));
            return r;                                  /* -1 done already, -2 name, -3 password */
        }
        case SYSTEM_SETUP_TEXT:
            users_setup_text();
            return 0;
        case SYSTEM_LOG: {
            if (!user_range_ok(a, sizeof(struct os_log), 1)) return SYSERR_FAULT;
            /* Read once into the kernel: another thread may change it meanwhile. */
            struct os_log l = *(const struct os_log *)a;
            if (l.max < 0 || !user_range_ok((uint64_t)(uintptr_t)l.buf, (uint64_t)l.max, 1)) return SYSERR_FAULT;
            uint64_t from = l.from;
            uint32_t n = print_log_read(&from, l.buf, (uint32_t)l.max);
            ((struct os_log *)a)->from = from;
            return n;
        }
        case SYSTEM_CPUS:
            return smp_cpu_count();
        case SYSTEM_PASSWD: {
            char old[64], now[64];
            if (!a) return users_has_password();         /* (a = 0: is a sign-in on?) */
            if (copy_user_string(old, a, sizeof(old)) != 0 || copy_user_string(now, b, sizeof(now)) != 0)
                return SYSERR_FAULT;
            int r = users_change_password(old, now);
            memset(old, 0, sizeof(old));
            memset(now, 0, sizeof(now));
            return r == 0 ? 0 : SYSERR_AUTH;
        }
        case SYSTEM_USERS: {
            int max = (int)b;
            if (!user_is_root()) return SYSERR_PERM;
            if (max < 1 || max > 16 || !user_range_ok(a, (uint64_t)max * sizeof(struct os_account), 1)) return SYSERR_FAULT;
            static char names[16][USER_NAME_MAX];
            unsigned uids[16];
            int has_pw[16];
            int n = users_list(names, uids, has_pw, max);
            struct os_account *out = (struct os_account *)a;
            for (int i = 0; i < n; i++) {
                memset(&out[i], 0, sizeof(out[i]));
                for (int k = 0; k < USER_NAME_MAX && names[i][k]; k++) out[i].name[k] = names[i][k];
                out[i].uid = uids[i];
                out[i].has_password = has_pw[i];
            }
            return n;
        }
        case SYSTEM_PAGES:
            return (long)((a ? physmem_total() - physmem_used() : physmem_total()) / 4096);
        case SYSTEM_POINTER: {
            if (!user_range_ok(a, sizeof(struct os_pointer), b ? 0 : 1)) return SYSERR_FAULT;
            if (b) mouse_set_settings((const struct os_pointer *)a);
            else mouse_get_settings((struct os_pointer *)a);
            return 0;
        }
        case SYSTEM_POWER:
            if (a == 2) {                                       /* sleep: back here at the wake-up */
                power_sleep();
                return 0;
            }
            print_set_graphics(0);
            vfs_shutdown();                                     /* ext3: clean, nothing to replay */
            if (a == 1) reboot();
            power_off();
            efi_reset(1);
            return SYSERR_IO;
        default:
            return SYSERR_BADCALL;
    }
}