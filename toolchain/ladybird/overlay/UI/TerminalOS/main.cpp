/*
 * Ladybird on Terminal OS: one program for the browser and its helpers.
 *
 * Ladybird starts a helper as this same file (Utilities.cpp's helper paths)
 * with the helper's name as argv[0]; that name picks its main here.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibMain/Main.h>
#include <sys/stat.h>
#include <unicode/putil.h>

#define HELPER(name) ErrorOr<int> name##_main(Main::Arguments);
HELPER(AudioServer)
HELPER(Compositor)
HELPER(ImageDecoder)
HELPER(MediaServer)
HELPER(RequestServer)
HELPER(WebContent)
HELPER(WebWorker)
#undef HELPER

namespace Ladybird {
ErrorOr<int> browser_main(Main::Arguments);
}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    // ICU's data is one file the whole system shares (icudt78l.dat): in /SYS
    // on an installed system, else at the top of the RAM disk.
    struct stat st;
    u_setDataDirectory(stat("/SYS/ICUDT78L.DAT", &st) == 0 ? "/SYS" : "/");

    struct Helper {
        StringView name;
        ErrorOr<int> (*main)(Main::Arguments);
    };
    static constexpr Helper helpers[] = {
        { "AudioServer"sv, AudioServer_main },
        { "Compositor"sv, Compositor_main },
        { "ImageDecoder"sv, ImageDecoder_main },
        { "MediaServer"sv, MediaServer_main },
        { "RequestServer"sv, RequestServer_main },
        { "WebContent"sv, WebContent_main },
        { "WebWorker"sv, WebWorker_main },
    };

    if (!arguments.strings.is_empty()) {
        auto name = LexicalPath::basename(arguments.strings[0]);
        for (auto const& helper : helpers) {
            if (name.equals_ignoring_ascii_case(helper.name))
                return helper.main(arguments);
        }
    }
    return Ladybird::browser_main(arguments);
}
