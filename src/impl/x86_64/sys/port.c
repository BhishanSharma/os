// port.c - ports: message queues and rights (sys/port.h)
#include "sys/port.h"
#include "sys/task.h"
#include "sys/process.h"
#include "sys/syscall_nums.h"
#include "drivers/heap.h"
#include "drivers/timer.h"
#include "lib/string.h"

#define MAX_PORTS   256
#define QUEUE_MAX   64
#define MAX_SERVICES 32

typedef struct msg {
    struct msg *next;
    uint32_t len;
    int reply;                    /* port index + 1 to give a SEND_ONCE right to, 0: none */
    uint8_t data[];
} msg_t;

typedef struct {
    int used, dead;               /* dead: its receiver let go; sends fail */
    int rights;                   /* names referring to it, in every program */
    msg_t *head, *tail;
    int count;
} port_t;

static port_t ports[MAX_PORTS];
static struct { char name[32]; int port; } services[MAX_SERVICES];

static void port_unref(int port) {
    port_t *p = &ports[port - 1];
    if (--p->rights > 0 || !p->dead) return;
    p->used = 0;
}

static int name_add(port_space_t *s, int port, int type) {
    if (type == PORT_RIGHT_SEND)                              /* one name per port for sending */
        for (int i = 0; i < PORT_RIGHTS; i++)
            if (s->r[i].port == port && s->r[i].type == PORT_RIGHT_SEND) return i;
    for (int i = 0; i < PORT_RIGHTS; i++)
        if (!s->r[i].port) {
            s->r[i].port = port;
            s->r[i].type = type;
            ports[port - 1].rights++;
            return i;
        }
    return SYSERR_MFILE;
}

static port_right_t *name_get(port_space_t *s, int name) {
    if (name < 0 || name >= PORT_RIGHTS || !s->r[name].port) return 0;
    return &s->r[name];
}

static void kill_port(int port) {
    port_t *p = &ports[port - 1];
    p->dead = 1;
    while (p->head) {
        msg_t *m = p->head;
        p->head = m->next;
        if (m->reply) port_unref(m->reply);
        kfree(m);
    }
    p->tail = 0;
    p->count = 0;
    for (int i = 0; i < MAX_SERVICES; i++)
        if (services[i].port == port) services[i].port = 0;
    task_wakeup(p);
}

int port_create(port_space_t *s) {
    for (int i = 0; i < MAX_PORTS; i++) {
        if (ports[i].used) continue;
        memset(&ports[i], 0, sizeof(ports[i]));
        ports[i].used = 1;
        int name = name_add(s, i + 1, PORT_RIGHT_RECEIVE);
        if (name < 0) ports[i].used = 0;
        return name;
    }
    return SYSERR_AGAIN;
}

static int interrupted(void) {
    return process_interrupted() || process_signal_pending();
}

int port_send(port_space_t *s, int name, const void *data, uint32_t len, int reply_name) {
    port_right_t *r = name_get(s, name);
    if (!r) return SYSERR_BADFD;
    if (len > PORT_MSG_MAX) return SYSERR_NOMEM;
    int reply = 0;
    if (reply_name >= 0) {
        port_right_t *rr = name_get(s, reply_name);
        if (!rr || rr->type == PORT_RIGHT_SEND_ONCE) return SYSERR_BADFD;
        reply = rr->port;
    }
    int port = r->port;
    port_t *p = &ports[port - 1];
    while (!p->dead && p->count >= QUEUE_MAX) {
        if (interrupted()) return SYSERR_TIMEOUT;
        task_block(p, 100);
    }
    if (p->dead) return SYSERR_DEAD;
    msg_t *m = kmalloc(sizeof(msg_t) + len);
    if (!m) return SYSERR_NOMEM;
    m->next = 0;
    m->len = len;
    m->reply = reply;
    if (reply) ports[reply - 1].rights++;                    /* the message holds it */
    memcpy(m->data, data, len);
    if (p->tail) p->tail->next = m;
    else p->head = m;
    p->tail = m;
    p->count++;
    task_wakeup(p);
    if (r->type == PORT_RIGHT_SEND_ONCE) port_close(s, name);
    return 0;
}

int port_receive(port_space_t *s, int name, void *buf, uint32_t max, uint32_t timeout_ms, int *reply_name) {
    port_right_t *r = name_get(s, name);
    if (!r || r->type != PORT_RIGHT_RECEIVE) return SYSERR_BADFD;
    port_t *p = &ports[r->port - 1];
    uint32_t start = get_tick();
    while (!p->head) {
        uint32_t waited = (get_tick() - start) * (1000 / TIMER_FREQ);
        if ((timeout_ms && waited >= timeout_ms) || interrupted()) return SYSERR_TIMEOUT;
        uint32_t step = timeout_ms && timeout_ms - waited < 100 ? timeout_ms - waited : 100;
        task_block(p, step ? step : 1);
    }
    msg_t *m = p->head;
    p->head = m->next;
    if (!p->head) p->tail = 0;
    p->count--;
    task_wakeup(p);                                          /* room for a waiting sender */
    uint32_t n = m->len < max ? m->len : max;
    memcpy(buf, m->data, n);
    *reply_name = -1;
    if (m->reply) {
        if (!ports[m->reply - 1].dead) *reply_name = name_add(s, m->reply, PORT_RIGHT_SEND_ONCE);
        port_unref(m->reply);
        if (*reply_name < 0) *reply_name = -1;
    }
    kfree(m);
    return (int)n;
}

int port_register(port_space_t *s, const char *service, int name) {
    port_right_t *r = name_get(s, name);
    if (!r) return SYSERR_BADFD;
    int slot = -1;
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (services[i].port && !strcmp(services[i].name, service)) { slot = i; break; }
        if (!services[i].port && slot < 0) slot = i;
    }
    if (slot < 0) return SYSERR_AGAIN;
    kstrncpy(services[slot].name, service, sizeof(services[slot].name));
    services[slot].port = r->port;
    return 0;
}

int port_lookup(port_space_t *s, const char *service) {
    for (int i = 0; i < MAX_SERVICES; i++)
        if (services[i].port && !strcmp(services[i].name, service) && !ports[services[i].port - 1].dead)
            return name_add(s, services[i].port, PORT_RIGHT_SEND);
    return SYSERR_NOENT;
}

int port_close(port_space_t *s, int name) {
    port_right_t *r = name_get(s, name);
    if (!r) return SYSERR_BADFD;
    int port = r->port;
    if (r->type == PORT_RIGHT_RECEIVE) kill_port(port);
    r->port = 0;
    port_unref(port);
    return 0;
}

void port_space_free(port_space_t *s) {
    for (int i = 0; i < PORT_RIGHTS; i++)
        if (s->r[i].port) port_close(s, i);
}
