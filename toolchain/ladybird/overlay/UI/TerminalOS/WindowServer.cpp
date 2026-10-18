/*
 * Ladybird on Terminal OS: a window of Terminal OS's window server.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <UI/TerminalOS/WindowServer.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace Ladybird {

// The window server's protocol (the OS's user/include/ws.h).
namespace WS {

static constexpr int CREATE = 1, TITLE = 2, RESIZE = 3, RESIZABLE = 4;
static constexpr int EVENTS = 64, HEADER = 4096, PREF_DARK = 1;

struct Request {
    int op;
    int w, h;
    int pid;
    int id;
    char title[48];
};

struct Reply {
    int id;
    char shm[32];
    int x, y;
    int w, h;
};

struct Surface {
    int w, h;
    unsigned volatile damage;
    unsigned volatile ev_head;
    unsigned volatile ev_tail;
    int volatile focused;
    unsigned volatile prefs;
    WindowEvent ev[EVENTS];
    int volatile dirty_x0, dirty_y0, dirty_x1, dirty_y1;
    unsigned volatile released;
};

static unsigned long surface_size(int w, int h) { return HEADER + (unsigned long)w * (unsigned long)h * 4; }

}

// Terminal OS system calls (the kernel's sys/syscall_nums.h).
static constexpr long SYS_SHMMAP = 42, SYS_PORT_CREATE = 48, SYS_PORT_SEND = 49, SYS_PORT_RECEIVE = 50,
                      SYS_PORT_LOOKUP = 52, SYS_PORT_CLOSE = 53;

static long syscall5(long n, long a, long b, long c, long d, long e)
{
    long r;
    register long r10 asm("r10") = d;
    register long r8 asm("r8") = e;
    asm volatile("int $0x80"
                 : "=a"(r)
                 : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}

static int reply_port = -1;

static int call(WS::Request const& request, WS::Reply& reply)
{
    long server = -1;
    for (int i = 0; i < 20 && (server = syscall5(SYS_PORT_LOOKUP, (long)"windowserver", 0, 0, 0, 0)) < 0; i++)
        usleep(50 * 1000);
    if (server < 0)
        return (int)server;
    if (reply_port < 0)
        reply_port = (int)syscall5(SYS_PORT_CREATE, 0, 0, 0, 0, 0);
    long r = syscall5(SYS_PORT_SEND, server, (long)&request, sizeof(request), reply_port, 0);
    syscall5(SYS_PORT_CLOSE, server, 0, 0, 0, 0);
    if (r < 0)
        return (int)r;
    int from;
    return (int)syscall5(SYS_PORT_RECEIVE, reply_port, (long)&reply, sizeof(reply), 5000, (long)&from);
}

static void send(WS::Request const& request)
{
    long server = syscall5(SYS_PORT_LOOKUP, (long)"windowserver", 0, 0, 0, 0);
    if (server < 0)
        return;
    syscall5(SYS_PORT_SEND, server, (long)&request, sizeof(request), -1, 0);
    syscall5(SYS_PORT_CLOSE, server, 0, 0, 0, 0);
}

ErrorOr<TOSWindow> TOSWindow::open(StringView title, int width, int height)
{
    WS::Request request {};
    request.op = WS::CREATE;
    request.w = width;
    request.h = height;
    request.pid = getpid();
    (void)title.copy_characters_to_buffer(request.title, sizeof(request.title));
    WS::Reply reply {};
    int n = call(request, reply);
    if (n < (int)sizeof(reply) || reply.id < 0)
        return Error::from_string_literal("The window server did not make a window");

    TOSWindow window;
    window.m_id = reply.id;
    window.m_width = reply.w;
    window.m_height = reply.h;
    long surface = syscall5(SYS_SHMMAP, (long)reply.shm, (long)WS::surface_size(reply.w, reply.h), 0, 0, 0);
    if (surface <= 0)
        return Error::from_string_literal("Could not map the window's surface");
    window.m_surface = (void*)surface;

    WS::Request resizable {};
    resizable.op = WS::RESIZABLE;
    resizable.id = reply.id;
    resizable.pid = getpid();
    send(resizable);
    return window;
}

u32* TOSWindow::pixels() const
{
    return (u32*)((char*)m_surface + WS::HEADER);
}

bool TOSWindow::is_dark() const
{
    return ((WS::Surface*)m_surface)->prefs & WS::PREF_DARK;
}

Optional<WindowEvent> TOSWindow::next_event()
{
    auto* s = (WS::Surface*)m_surface;
    if (s->ev_tail == s->ev_head)
        return {};
    WindowEvent event = s->ev[s->ev_tail % WS::EVENTS];
    s->ev_tail = s->ev_tail + 1;
    // Moves with no button down: only the latest matters.
    while (event.type == WindowEvent::MouseMove && !event.buttons && s->ev_tail != s->ev_head
        && s->ev[s->ev_tail % WS::EVENTS].type == WindowEvent::MouseMove && !s->ev[s->ev_tail % WS::EVENTS].buttons) {
        event = s->ev[s->ev_tail % WS::EVENTS];
        s->ev_tail = s->ev_tail + 1;
    }
    return event;
}

void TOSWindow::set_title(StringView title)
{
    WS::Request request {};
    request.op = WS::TITLE;
    request.id = m_id;
    request.pid = getpid();
    (void)title.copy_characters_to_buffer(request.title, sizeof(request.title));
    send(request);
}

void TOSWindow::damage(int x, int y, int w, int h)
{
    auto* s = (WS::Surface*)m_surface;
    int x0 = max(x, 0), y0 = max(y, 0), x1 = min(x + w, m_width), y1 = min(y + h, m_height);
    if (x1 <= x0 || y1 <= y0)
        return;
    if (s->dirty_x1 <= s->dirty_x0) {
        s->dirty_x0 = x0;
        s->dirty_y0 = y0;
        s->dirty_x1 = x1;
        s->dirty_y1 = y1;
    } else {
        s->dirty_x0 = min(s->dirty_x0, x0);
        s->dirty_y0 = min(s->dirty_y0, y0);
        s->dirty_x1 = max(s->dirty_x1, x1);
        s->dirty_y1 = max(s->dirty_y1, y1);
    }
}

void TOSWindow::present()
{
    // A new frame: the server copies the surface when it next draws. No
    // waiting for it here: this is the browser's main thread, and a frame
    // drawn over one not yet copied is just the newer picture.
    auto* s = (WS::Surface*)m_surface;
    s->damage = s->damage + 1;
}

ErrorOr<void> TOSWindow::resize(int width, int height)
{
    WS::Request request {};
    request.op = WS::RESIZE;
    request.id = m_id;
    request.pid = getpid();
    request.w = width;
    request.h = height;
    WS::Reply reply {};
    int n = call(request, reply);
    if (n < (int)sizeof(reply) || reply.id < 0)
        return Error::from_string_literal("The window server did not resize the window");
    long surface = syscall5(SYS_SHMMAP, (long)reply.shm, (long)WS::surface_size(reply.w, reply.h), 0, 0, 0);
    if (surface <= 0)
        return Error::from_string_literal("Could not map the window's new surface");
    m_surface = (void*)surface;
    m_width = reply.w;
    m_height = reply.h;
    return {};
}

}
