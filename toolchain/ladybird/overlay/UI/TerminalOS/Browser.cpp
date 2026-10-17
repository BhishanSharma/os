/*
 * Ladybird on Terminal OS: the browser process (the user interface).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMain/Main.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowserProcess.h>
#include <UI/TerminalOS/Browser.h>

namespace Ladybird {


ErrorOr<int> browser_main(Main::Arguments arguments)
{
    auto app = TRY(Application::create(arguments));
    if (app->should_exit_after_profile_coordination())
        return 0;

    if (!Application::browser_options().headless_mode.has_value())
        TRY(app->open_window());

    return app->execute();
}

}
