/*
 * Ladybird on Terminal OS: the browser window. A toolbar (back, forward,
 * reload, the address) drawn with Skia, and under it the page: the frames
 * WebContent paints, copied into the window's surface. Input goes to the
 * page, or to the address while it is being edited.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/SystemTheme.h>
#include <LibURL/URL.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/URL.h>
#include <LibWebView/Utilities.h>
#include <UI/TerminalOS/Browser.h>
#include <UI/TerminalOS/WindowServer.h>

#include <core/SkCanvas.h>
#include <core/SkData.h>
#include <core/SkFont.h>
#include <core/SkFontMgr.h>
#include <core/SkPaint.h>
#include <core/SkRRect.h>
#include <core/SkSurface.h>
#include <core/SkTypeface.h>
#include <ports/SkFontMgr_empty.h>

namespace Ladybird {

static constexpr int TOOLBAR_HEIGHT = 40;
static constexpr u32 KEY_UP = 0x101, KEY_DOWN = 0x102, KEY_LEFT = 0x103, KEY_RIGHT = 0x104;

// The page: a web view whose frames go into the window.
class TOSWebView final : public WebView::HeadlessWebView {
public:
    static NonnullOwnPtr<TOSWebView> create(Core::AnonymousBuffer theme, Web::DevicePixelSize size)
    {
        auto view = adopt_own(*new TOSWebView(move(theme), size));
        view->initialize_tab(Web::HTML::VisibilityState::Visible);
        return view;
    }

    // The last frame, if there is one.
    Gfx::Bitmap const* frame() const
    {
        if (!m_has_usable_bitmap || !m_front_bitmap.shared_image_buffer)
            return nullptr;
        return m_front_bitmap.shared_image_buffer->bitmap().ptr();
    }

    void resize(Web::DevicePixelSize size) { reset_viewport_size(size); }

private:
    TOSWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize size)
        : HeadlessWebView(move(theme), size)
    {
    }
};

class BrowserWindow {
    AK_ALLOC_WITH_KMALLOC;

public:
    static ErrorOr<NonnullOwnPtr<BrowserWindow>> create(Vector<URL::URL> const& urls)
    {
        auto window = TRY(TOSWindow::open("Ladybird"sv, 1100, 760));
        auto browser = adopt_own(*new BrowserWindow(move(window)));
        TRY(browser->initialize(urls));
        return browser;
    }

    WebView::ViewImplementation& view() { return *m_view; }

private:
    explicit BrowserWindow(TOSWindow window)
        : m_window(move(window))
    {
    }

    ErrorOr<void> initialize(Vector<URL::URL> const& urls)
    {
        auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
        auto theme = TRY(Gfx::load_system_theme(theme_path.string()));
        m_view = TOSWebView::create(move(theme), page_size());

        load_font();

        m_view->on_ready_to_paint = [this] { draw_page(); };
        m_view->on_title_change = [this](Utf16String const& title) {
            auto text = title.to_utf8();
            m_window.set_title(text.is_empty() ? "Ladybird"sv : text.bytes_as_string_view());
        };
        m_view->on_url_change = [this](URL::URL const& url) {
            if (!m_editing) {
                m_address = url.serialize().to_byte_string();
                draw_toolbar();
            }
        };
        m_view->on_load_start = [this] {
            m_loading = true;
            draw_toolbar();
        };
        m_view->on_load_finish = [this](URL::URL const&) {
            m_loading = false;
            draw_toolbar();
        };

        draw_toolbar();
        clear_page();
        m_window.present();

        m_event_timer = Core::Timer::create_repeating(8, [this] { pump_events(); });
        m_event_timer->start();

        // (On a first run Ladybird puts about:welcome before the URL asked for: open the latter.)
        auto start = urls.is_empty() ? URL::URL {} : urls.last();
        if (urls.is_empty()) {
            m_editing = true;
            m_address = {};
            draw_toolbar();
            m_window.present();
        } else {
            m_view->load(start);
        }
        return {};
    }

    Web::DevicePixelSize page_size() const
    {
        return { m_window.width(), max(1, m_window.height() - TOOLBAR_HEIGHT) };
    }

    // ---- Drawing ----

    void load_font()
    {
        auto program = Core::System::current_executable_path();
        if (program.is_error())
            return;
        auto prefix = LexicalPath(LexicalPath(program.value()).dirname()).dirname();
        auto manager = SkFontMgr_New_Custom_Empty();
        for (auto name : { "Inter.ttf"sv, "INTER.TTF"sv, "SerenitySans-Regular.ttf"sv }) {
            auto path = ByteString::formatted("{}/share/fonts/{}", prefix, name);
            if (auto data = SkData::MakeFromFileName(path.characters())) {
                m_typeface = manager->makeFromData(move(data));
                if (m_typeface)
                    break;
            }
        }
    }

    sk_sp<SkSurface> surface()
    {
        auto info = SkImageInfo::Make(m_window.width(), m_window.height(), kBGRA_8888_SkColorType, kOpaque_SkAlphaType);
        return SkSurfaces::WrapPixels(info, m_window.pixels(), m_window.width() * 4);
    }

    struct Colors {
        SkColor bar, field, field_border, text, faint, accent;
    };

    Colors colors() const
    {
        if (m_window.is_dark())
            return { 0xFF2B2B30, 0xFF1C1C1F, 0xFF4A4A52, 0xFFEDEDED, 0xFF9A9AA2, 0xFF4C8DFF };
        return { 0xFFF1F1F4, 0xFFFFFFFF, 0xFFC9C9D1, 0xFF1D1D1F, 0xFF707078, 0xFF2A6CF0 };
    }

    // Buttons: back, forward, reload; then the address field.
    static constexpr int BUTTON = 30, FIELD_X = 8 + 3 * (BUTTON + 4) + 6;

    void draw_toolbar()
    {
        auto s = surface();
        if (!s)
            return;
        auto* canvas = s->getCanvas();
        auto c = colors();
        int w = m_window.width();

        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setColor(c.bar);
        canvas->drawRect(SkRect::MakeXYWH(0, 0, w, TOOLBAR_HEIGHT), paint);
        paint.setColor(c.field_border);
        canvas->drawRect(SkRect::MakeXYWH(0, TOOLBAR_HEIGHT - 1, w, 1), paint);

        SkFont font(m_typeface, 14);
        SkFont icons(m_typeface, 17);
        char const* labels[] = { "\xE2\x86\x90", "\xE2\x86\x92", m_loading ? "\xC3\x97" : "\xE2\x86\xBB" };   // ← → × ↻
        char const* fallback[] = { "<", ">", m_loading ? "x" : "R" };
        for (int i = 0; i < 3; i++) {
            int x = 8 + i * (BUTTON + 4);
            paint.setColor(c.field);
            canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, 5, BUTTON, BUTTON), 6, 6), paint);
            paint.setColor(c.text);
            char const* label = m_typeface && m_typeface->unicharToGlyph(i == 2 && !m_loading ? 0x21BB : 0x2190) ? labels[i] : fallback[i];
            SkRect bounds;
            icons.measureText(label, strlen(label), SkTextEncoding::kUTF8, &bounds);
            canvas->drawSimpleText(label, strlen(label), SkTextEncoding::kUTF8, x + (BUTTON - bounds.width()) / 2 - bounds.left(), 26, icons, paint);
        }

        int fx = FIELD_X, fw = w - fx - 8;
        paint.setColor(c.field);
        auto field = SkRRect::MakeRectXY(SkRect::MakeXYWH(fx, 5, fw, 30), 8, 8);
        canvas->drawRRect(field, paint);
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(m_editing ? 2 : 1);
        paint.setColor(m_editing ? c.accent : c.field_border);
        canvas->drawRRect(field, paint);
        paint.setStyle(SkPaint::kFill_Style);

        canvas->save();
        canvas->clipRect(SkRect::MakeXYWH(fx + 10, 5, fw - 20, 30));
        bool empty = m_address.is_empty();
        auto text = empty && m_editing ? ByteString("Search or enter an address") : m_address;
        paint.setColor(empty ? c.faint : c.text);
        canvas->drawSimpleText(text.characters(), text.length(), SkTextEncoding::kUTF8, fx + 12, 25, font, paint);
        if (m_editing) {
            float caret = empty ? 0 : font.measureText(m_address.characters(), m_address.length(), SkTextEncoding::kUTF8);
            paint.setColor(c.accent);
            canvas->drawRect(SkRect::MakeXYWH(fx + 12 + caret + 1, 11, 2, 19), paint);
        }
        canvas->restore();

        m_window.damage(0, 0, w, TOOLBAR_HEIGHT);
    }

    void clear_page()
    {
        u32* px = m_window.pixels();
        u32 color = m_window.is_dark() ? 0x1C1C1F : 0xFFFFFF;
        for (int y = TOOLBAR_HEIGHT; y < m_window.height(); y++)
            for (int x = 0; x < m_window.width(); x++)
                px[y * m_window.width() + x] = color;
        m_window.damage(0, TOOLBAR_HEIGHT, m_window.width(), m_window.height() - TOOLBAR_HEIGHT);
    }

    void draw_page()
    {
        auto const* frame = m_view->frame();
        if (!frame)
            return;
        int w = min(frame->width(), m_window.width());
        int h = min(frame->height(), m_window.height() - TOOLBAR_HEIGHT);
        u32* out = m_window.pixels();
        for (int y = 0; y < h; y++) {
            auto const* row = frame->scanline(y);
            u32* dst = out + (y + TOOLBAR_HEIGHT) * m_window.width();
            for (int x = 0; x < w; x++)
                dst[x] = row[x] & 0xFFFFFF;
        }
        m_window.damage(0, TOOLBAR_HEIGHT, w, h);
        m_window.present();
    }

    // ---- Input ----

    void pump_events()
    {
        bool toolbar_changed = false;
        for (Optional<WindowEvent> event; (event = m_window.next_event()).has_value();) {
            switch (event->type) {
            case WindowEvent::Key:
                toolbar_changed |= handle_key(event->key);
                break;
            case WindowEvent::MouseDown:
            case WindowEvent::MouseUp:
            case WindowEvent::MouseMove:
                toolbar_changed |= handle_mouse(*event);
                break;
            case WindowEvent::Wheel:
                if (event->y >= TOOLBAR_HEIGHT)
                    send_mouse(Web::MouseEvent::Type::MouseWheel, *event, 0, event->key * 3 * 16.0);
                break;
            case WindowEvent::Resize:
                if (!m_window.resize(event->x, event->y).is_error()) {
                    m_view->resize(page_size());
                    draw_toolbar();
                    clear_page();
                    draw_page();
                }
                break;
            case WindowEvent::Preferences:
                toolbar_changed = true;
                break;
            case WindowEvent::Close:
                Core::EventLoop::current().quit(0);
                return;
            }
        }
        if (toolbar_changed) {
            draw_toolbar();
            m_window.present();
        }
    }

    static Web::UIEvents::MouseButton buttons_of(int buttons)
    {
        unsigned b = 0;
        if (buttons & 1)
            b |= to_underlying(Web::UIEvents::MouseButton::Primary);
        if (buttons & 2)
            b |= to_underlying(Web::UIEvents::MouseButton::Secondary);
        if (buttons & 4)
            b |= to_underlying(Web::UIEvents::MouseButton::Middle);
        return static_cast<Web::UIEvents::MouseButton>(b);
    }

    void send_mouse(Web::MouseEvent::Type type, WindowEvent const& event, double dx, double dy)
    {
        Web::DevicePixelPoint position { event.x, event.y - TOOLBAR_HEIGHT };
        auto buttons = buttons_of(event.buttons);
        auto button = Web::UIEvents::MouseButton::None;
        if (type == Web::MouseEvent::Type::MouseDown || type == Web::MouseEvent::Type::MouseUp) {
            auto changed = to_underlying(buttons) ^ to_underlying(m_buttons);
            button = changed ? static_cast<Web::UIEvents::MouseButton>(changed & -changed) : Web::UIEvents::MouseButton::Primary;
            if (type == Web::MouseEvent::Type::MouseDown)
                m_click_count = 1;
        }
        m_buttons = buttons;
        Web::MouseEvent mouse {};
        mouse.type = type;
        mouse.position = position;
        mouse.screen_position = position;
        mouse.button = button;
        mouse.buttons = buttons;
        mouse.wheel_delta_x = dx;
        mouse.wheel_delta_y = dy;
        mouse.click_count = m_click_count;
        m_view->enqueue_input_event(move(mouse));
    }

    bool handle_mouse(WindowEvent const& event)
    {
        using Type = Web::MouseEvent::Type;
        auto type = event.type == WindowEvent::MouseDown ? Type::MouseDown
            : event.type == WindowEvent::MouseUp         ? Type::MouseUp
                                                         : Type::MouseMove;
        if (event.y < TOOLBAR_HEIGHT && !m_dragging_page) {
            if (type != Type::MouseDown)
                return false;
            for (int i = 0; i < 3; i++) {
                int x = 8 + i * (BUTTON + 4);
                if (event.x >= x && event.x < x + BUTTON) {
                    if (i == 0)
                        m_view->traverse_the_history_by_delta(-1);
                    else if (i == 1)
                        m_view->traverse_the_history_by_delta(1);
                    else
                        m_view->reload();
                    return false;
                }
            }
            if (event.x >= FIELD_X) {
                m_editing = true;
                return true;
            }
            return false;
        }
        bool changed = false;
        if (type == Type::MouseDown) {
            m_dragging_page = true;
            if (m_editing) {
                m_editing = false;
                changed = true;
            }
        } else if (type == Type::MouseUp) {
            m_dragging_page = false;
        }
        send_mouse(type, event, 0, 0);
        return changed;
    }

    bool handle_key(int key)
    {
        if (m_editing) {
            if (key == '\n' || key == '\r') {
                if (auto url = WebView::sanitize_url(m_address, WebView::Application::settings().search_engine_settings().engine); url.has_value()) {
                    m_editing = false;
                    m_address = url->serialize().to_byte_string();
                    m_view->load(*url);
                }
            } else if (key == 8 || key == 127) {
                if (!m_address.is_empty())
                    m_address = m_address.substring(0, utf8_start_of_last(m_address));
            } else if (key == 27) {
                m_editing = false;
                m_address = m_view->url().serialize().to_byte_string();
            } else if (key == 21) {                     // Ctrl+U: clear
                m_address = {};
            } else if (key >= 32 && key < 0x100 && key != 127) {
                StringBuilder builder;
                builder.append(m_address);
                builder.append_code_point(key);
                m_address = builder.to_byte_string();
            }
            return true;
        }
        if (key == 12) {                                // Ctrl+L: the address
            m_editing = true;
            m_address = {};
            return true;
        }
        send_key(key);
        return false;
    }

    static size_t utf8_start_of_last(ByteString const& s)
    {
        size_t i = s.length() - 1;
        while (i > 0 && (s[i] & 0xC0) == 0x80)
            i--;
        return i;
    }

    void send_key(int key)
    {
        using namespace Web::UIEvents;
        KeyCode code = KeyCode::Key_Invalid;
        u32 code_point = 0;
        auto modifiers = KeyModifier::Mod_None;
        switch (key) {
        case KEY_UP: code = KeyCode::Key_Up; break;
        case KEY_DOWN: code = KeyCode::Key_Down; break;
        case KEY_LEFT: code = KeyCode::Key_Left; break;
        case KEY_RIGHT: code = KeyCode::Key_Right; break;
        case '\n':
        case '\r': code = KeyCode::Key_Return; code_point = '\r'; break;
        case 8: code = KeyCode::Key_Backspace; break;
        case 9: code = KeyCode::Key_Tab; code_point = '\t'; break;
        case 27: code = KeyCode::Key_Escape; break;
        case 127: code = KeyCode::Key_Delete; break;
        default:
            if (key >= 1 && key <= 26) {                // Ctrl+letter
                code = static_cast<KeyCode>(to_underlying(KeyCode::Key_A) + key - 1);
                modifiers = KeyModifier::Mod_Ctrl;
            } else if (key >= 32 && key < 0x100) {
                code_point = key;
                code = code_for_character(key);
            }
            break;
        }
        bool insert = code_point >= 32 && modifiers == KeyModifier::Mod_None;
        Web::KeyEvent down { Web::KeyEvent::Type::KeyDown, code, modifiers, code_point, false, insert, nullptr };
        m_view->enqueue_input_event(move(down));
        Web::KeyEvent up { Web::KeyEvent::Type::KeyUp, code, modifiers, code_point, false, false, nullptr };
        m_view->enqueue_input_event(move(up));
    }

    static Web::UIEvents::KeyCode code_for_character(int c)
    {
        using Web::UIEvents::KeyCode;
        if (c >= 'a' && c <= 'z')
            return static_cast<KeyCode>(to_underlying(KeyCode::Key_A) + c - 'a');
        if (c >= 'A' && c <= 'Z')
            return static_cast<KeyCode>(to_underlying(KeyCode::Key_A) + c - 'A');
        if (c >= '0' && c <= '9')
            return static_cast<KeyCode>(to_underlying(KeyCode::Key_0) + c - '0');
        if (c == ' ')
            return KeyCode::Key_Space;
        return KeyCode::Key_Invalid;
    }

    TOSWindow m_window;
    OwnPtr<TOSWebView> m_view;
    sk_sp<SkTypeface> m_typeface;
    RefPtr<Core::Timer> m_event_timer;
    ByteString m_address;
    bool m_editing { false };
    bool m_loading { false };
    bool m_dragging_page { false };
    Web::UIEvents::MouseButton m_buttons { Web::UIEvents::MouseButton::None };
    int m_click_count { 0 };
};

Application::Application() = default;
Application::~Application() = default;

ErrorOr<void> Application::open_window()
{
    m_window = TRY(BrowserWindow::create(browser_options().urls));
    return {};
}

Optional<WebView::ViewImplementation&> Application::active_web_view() const
{
    if (!m_window)
        return {};
    return m_window->view();
}

}
