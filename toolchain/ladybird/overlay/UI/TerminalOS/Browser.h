/*
 * Ladybird on Terminal OS: the browser process (the user interface).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibWebView/Application.h>

namespace Ladybird {

class BrowserWindow;

class Application final : public WebView::Application {
    WEB_VIEW_APPLICATION(Application)

public:
    virtual ~Application() override;

    // The window: an address bar and the page (WebViewTOS.cpp).
    ErrorOr<void> open_window();

    virtual Optional<WebView::ViewImplementation&> active_web_view() const override;

private:
    Application();

    virtual bool should_coordinate_browser_process() const override { return false; }

    OwnPtr<BrowserWindow> m_window;
};

}
