/*
 * Ladybird on Terminal OS: a window of Terminal OS's window server.
 *
 * The protocol (user/include/ws.h in the OS): a request to the
 * "windowserver" port makes the window; its pixels and an event ring are a
 * shared memory surface, mapped by name.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Types.h>

namespace Ladybird {

struct WindowEvent {
    enum Type {
        Key = 1,
        MouseDown,
        MouseUp,
        MouseMove,
        Wheel,
        Close,
        Resize,
        Preferences,
    };
    int type;
    int x, y;              // in the content; Resize: the new size
    int key;               // Key: the key code (ASCII; 0x101-0x10A arrows, Home, End, PgUp, PgDn, Delete, Insert,
                           // with 0x1000 Shift and 0x2000 Ctrl); Wheel: steps
    int buttons;
};

// The clipboard every program of the user shares (the OS's user/lib/ui.c).
ByteString clipboard_get();
void clipboard_set(StringView);

class TOSWindow {
public:
    static ErrorOr<TOSWindow> open(StringView title, int width, int height);

    int width() const { return m_width; }
    int height() const { return m_height; }
    u32* pixels() const;          // width * height, 0x00RRGGBB
    bool is_dark() const;

    Optional<WindowEvent> next_event();
    void set_title(StringView);
    void damage(int x, int y, int w, int h);
    void present();
    ErrorOr<void> resize(int width, int height);   // after a Resize event

private:
    TOSWindow() = default;

    void* m_surface { nullptr };
    int m_id { -1 };
    int m_width { 0 };
    int m_height { 0 };
};

}
