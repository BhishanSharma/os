// syscall_usock.c - local sockets (from sys/process.c): a pipe each way,
// named by a path, with accept() and passing open files across.
#include "process_internal.h"
#include "sys/process.h"
#include "sys/task.h"
#include "drivers/heap.h"
#include "lib/string.h"

static file_t *listeners[MAX_LISTENERS];       /* F_ULISTEN files; their names are f->path */

static usock_t *usock_new(void) {
    usock_t *us = kmalloc(sizeof(*us));
    if (!us) return 0;
    memset(us, 0, sizeof(*us));
    us->pipe[0] = pipe_new();                    /* (one reader, one writer: the two ends) */
    us->pipe[1] = pipe_new();
    if (!us->pipe[0] || !us->pipe[1]) {
        kfree(us->pipe[0]);
        kfree(us->pipe[1]);
        kfree(us);
        return 0;
    }
    us->ends = 2;
    return us;
}

static void usock_free(usock_t *us) {
    if (!us) return;
    kfree(us->pipe[0]);
    kfree(us->pipe[1]);
    kfree(us);
}

void usock_closed(file_t *f) {
    usock_t *us = f->us;
    if (!us) return;
    ends_closed(f);
    if (--us->ends > 0) return;
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < us->fdq_n[s]; i++) file_put(us->fdq[s][i]);
    kfree(us->pty);
    kfree(us);
}

/* ---- Pseudo-terminals ---------------------------------------------------- */

/* A connected pair, as USOCK_PAIR; with a pty, side 0 the master. */
static int64_t new_pair(uint64_t fds, int pty) {
    if (!user_range_ok(fds, 2 * sizeof(int), 1)) return SYSERR_FAULT;
    usock_t *us = usock_new();
    file_t *x = file_new(F_USOCK), *y = file_new(F_USOCK);
    pty_t *t = pty ? kmalloc(sizeof(pty_t)) : 0;
    if (!us || !x || !y || (pty && !t)) {
        usock_free(us);
        kfree(x);
        kfree(y);
        kfree(t);
        return SYSERR_NOMEM;
    }
    if (t) {                                     /* cooked, echo, Ctrl+C; 80 x 25 */
        memset(t, 0, sizeof(*t));
        t->t.iflag = 0400;                       /* ICRNL */
        t->t.oflag = 05;                         /* OPOST | ONLCR */
        t->t.cflag = 0277;                       /* CS8 | CREAD | B38400 */
        t->t.lflag = PTY_ISIG | PTY_ICANON | PTY_ECHO | 0x10 | 0x20 | 0x8000;   /* ECHOE ECHOK IEXTEN */
        static const unsigned char cc[] = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22 };
        memcpy(t->t.cc, cc, sizeof(cc));
        t->ws.rows = 25;
        t->ws.cols = 80;
        us->pty = t;
    }
    x->us = y->us = us;
    y->side = 1;
    int64_t fx = install(x);
    if (fx < 0) {
        file_put(y);
        return fx;
    }
    int64_t fy = install(y);
    if (fy < 0) {
        sys_close((uint64_t)fx);
        return fy;
    }
    ((int *)fds)[0] = (int)fx;
    ((int *)fds)[1] = (int)fy;
    return 0;
}

static int64_t pty_op(uint64_t op, file_t *f, uint64_t b) {
    pty_t *t = f->kind == F_USOCK && f->us ? f->us->pty : 0;
    if (!t) return SYSERR_BADFD;                        /* (ENOTTY) */
    switch (op) {
    case USOCK_TCGET:
        if (!user_range_ok(b, sizeof(struct os_termios), 1)) return SYSERR_FAULT;
        memcpy((void *)b, &t->t, sizeof(t->t));
        return 0;
    case USOCK_TCSET:
        if (!user_range_ok(b, sizeof(struct os_termios), 0)) return SYSERR_FAULT;
        memcpy(&t->t, (const void *)b, sizeof(t->t));
        t->fg = task_current()->pid;
        io_event();                                     /* (the window looks at the mode) */
        return 0;
    case USOCK_WINSZ:
        if (!user_range_ok(b, sizeof(struct os_winsize), 1)) return SYSERR_FAULT;
        memcpy((void *)b, &t->ws, sizeof(t->ws));
        return 0;
    case USOCK_SETWINSZ: {
        if (!user_range_ok(b, sizeof(struct os_winsize), 0)) return SYSERR_FAULT;
        struct os_winsize ws;
        memcpy(&ws, (const void *)b, sizeof(ws));
        int changed = ws.rows != t->ws.rows || ws.cols != t->ws.cols;
        t->ws = ws;
        if (changed && t->fg && signal_handled(t->fg, SIGWINCH)) signal_send(t->fg, SIGWINCH);
        return 0;
    }
    }
    return SYSERR_BADCALL;
}

void listener_closed(file_t *f) {
    for (int i = 0; i < MAX_LISTENERS; i++)
        if (listeners[i] == f) listeners[i] = 0;
    ulisten_t *l = f->ul;
    for (int i = 0; l && i < l->n; i++) file_put(l->q[i]);
    kfree(l);
}

static file_t *listener_named(const char *name) {
    for (int i = 0; i < MAX_LISTENERS; i++)
        if (listeners[i] && strcmp(listeners[i]->path, name) == 0) return listeners[i];
    return 0;
}

int64_t sys_usock(uint64_t op, uint64_t a, uint64_t b, uint64_t c) {
    process_t *p = P();
    if (op == USOCK_NEW) {
        file_t *f = file_new(F_USOCK);
        return f ? install(f) : SYSERR_NOMEM;
    }
    if (op == USOCK_PAIR || op == USOCK_PTY) return new_pair(a, op == USOCK_PTY);
    file_t *f = fd_get(a);
    if (!f) return SYSERR_BADFD;
    if (op >= USOCK_TCGET && op <= USOCK_SETWINSZ) return pty_op(op, f, b);
    switch (op) {
    case USOCK_NONBLOCK:
        if (f->kind == F_FILE || f->kind == F_CONSOLE) return SYSERR_BADFD;
        f->nonblock = b != 0;
        return 0;
    case USOCK_NAME:
        if (f->kind != F_USOCK && f->kind != F_ULISTEN) return SYSERR_BADFD;
        if (!user_range_ok(b, 108, 1)) return SYSERR_FAULT;
        kstrncpy((char *)b, f->path, 108);
        return 0;
    case USOCK_BIND:
    case USOCK_CONNECT: {
        if (f->kind != F_USOCK || f->us || f->path[0]) return SYSERR_BADFD;
        char name[MAX_PATH];
        if (user_path(b, name, sizeof(name)) != 0) return SYSERR_FAULT;
        if (op == USOCK_BIND) {
            if (listener_named(name)) return SYSERR_INUSE;
            memcpy(f->path, name, sizeof(name));
            return 0;
        }
        file_t *lf = listener_named(name);
        if (!lf || lf->ul->n >= BACKLOG) return SYSERR_REFUSED;
        usock_t *us = usock_new();
        file_t *server = file_new(F_USOCK);
        if (!us || !server) {
            usock_free(us);
            kfree(server);
            return SYSERR_NOMEM;
        }
        f->us = server->us = us;
        server->side = 1;
        memcpy(f->path, name, sizeof(name));
        memcpy(server->path, name, sizeof(name));
        lf->ul->q[lf->ul->n++] = server;
        task_wakeup(lf);
        io_event();
        return 0;
    }
    case USOCK_LISTEN: {
        if (f->kind == F_ULISTEN) return 0;
        if (f->kind != F_USOCK || f->us || !f->path[0]) return SYSERR_BADFD;
        if (listener_named(f->path)) return SYSERR_INUSE;
        int slot = -1;
        for (int i = 0; i < MAX_LISTENERS && slot < 0; i++)
            if (!listeners[i]) slot = i;
        if (slot < 0) return SYSERR_MFILE;
        f->ul = kmalloc(sizeof(ulisten_t));
        if (!f->ul) return SYSERR_NOMEM;
        memset(f->ul, 0, sizeof(ulisten_t));
        f->kind = F_ULISTEN;
        listeners[slot] = f;
        return 0;
    }
    case USOCK_ACCEPT: {
        if (f->kind != F_ULISTEN) return SYSERR_BADFD;
        ulisten_t *l = f->ul;
        while (l->n == 0) {
            if (f->nonblock) return SYSERR_WOULDBLOCK;
            if (process_interrupted() || process_signal_pending()) return SYSERR_TIMEOUT;
            task_block(f, 100);
        }
        if (fd_free_slot(p, 0) < 0) return SYSERR_MFILE;
        file_t *conn = l->q[0];
        for (int i = 1; i < l->n; i++) l->q[i - 1] = l->q[i];
        l->n--;
        return install(conn);
    }
    case USOCK_SENDFDS: {
        if (f->kind != F_USOCK || !f->us) return SYSERR_BADFD;
        if (c > USOCK_MAX_FDS || !user_range_ok(b, c * sizeof(int), 0)) return SYSERR_FAULT;
        usock_t *us = f->us;
        int to = !f->side;
        if (us->fdq_n[to] + (int)c > USOCK_MAX_FDS) return SYSERR_MFILE;
        const int *fds = (const int *)b;
        for (uint64_t i = 0; i < c; i++)
            if (fds[i] < 0 || !fd_get((uint64_t)fds[i])) return SYSERR_BADFD;
        for (uint64_t i = 0; i < c; i++) {
            file_t *x = file_get(fd_get((uint64_t)fds[i]));
            ends_added(x);
            us->fdq[to][us->fdq_n[to]++] = x;
        }
        io_event();
        return 0;
    }
    case USOCK_RECVFDS: {
        if (f->kind != F_USOCK || !f->us) return SYSERR_BADFD;
        if (!user_range_ok(b, c * sizeof(int), 1)) return SYSERR_FAULT;
        usock_t *us = f->us;
        int me = f->side, n = 0;
        int *out = (int *)b;
        while ((uint64_t)n < c && us->fdq_n[me] > 0) {
            int fd = fd_free_slot(p, 0);
            if (fd < 0) break;
            p->fds[fd] = us->fdq[me][0];
            for (int i = 1; i < us->fdq_n[me]; i++) us->fdq[me][i - 1] = us->fdq[me][i];
            us->fdq_n[me]--;
            out[n++] = fd;
        }
        return n;
    }
    }
    return SYSERR_BADCALL;
}
