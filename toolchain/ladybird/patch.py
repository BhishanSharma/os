#!/usr/bin/env python3
# Ladybird for Terminal OS: the edits to Ladybird's own files. Run in the
# source tree (toolchain/ladybird/build.sh does, then copies overlay/ over it).
# Each edit replaces text that must occur exactly once; an edit already made
# is skipped, so running it twice is harmless.
import sys

failed = False


def edit(path, old, new):
    global failed
    s = open(path, encoding='utf-8').read()
    if new in s and old not in s:
        return
    if s.count(old) != 1:
        print(f'patch.py: {path}: found {s.count(old)} times: {old[:60]!r}')
        failed = True
        return
    open(path, 'w', encoding='utf-8').write(s.replace(old, new))


# The OS: AK_OS_TERMINALOS (GCC defines __terminalos__); otherwise the
# generic POSIX paths.
edit('AK/Platform.h', '''#if defined(__HAIKU__)
#    define AK_OS_HAIKU
#endif''', '''#if defined(__HAIKU__)
#    define AK_OS_HAIKU
#endif

#if defined(__terminalos__)
#    define AK_OS_TERMINALOS
#endif''')

# No command-line utilities or tests: the browser only.
edit('CMakeLists.txt', 'OR ANDROID OR IOS))\n    add_subdirectory(Utilities)',
     'OR ANDROID OR IOS OR TERMINALOS))\n    add_subdirectory(Utilities)')

# Dependencies Terminal OS goes without: libedit (the js REPL), fontconfig
# (fonts come from a folder), SDL3 (gamepads).
edit('Meta/CMake/check_for_dependencies.cmake', 'if (NOT WIN32 AND NOT ANDROID)\n    pkg_check_modules(LIBEDIT',
     'if (NOT WIN32 AND NOT ANDROID AND NOT TERMINALOS)\n    pkg_check_modules(LIBEDIT')
edit('Meta/CMake/check_for_dependencies.cmake', 'if (NOT APPLE AND NOT ANDROID AND NOT WIN32)\n    find_package(Fontconfig',
     'if (NOT APPLE AND NOT ANDROID AND NOT WIN32 AND NOT TERMINALOS)\n    find_package(Fontconfig')
edit('Meta/CMake/check_for_dependencies.cmake', 'find_package(SDL3 CONFIG REQUIRED)\n',
     'if (NOT TERMINALOS)\n    find_package(SDL3 CONFIG REQUIRED)\nendif()\n')
edit('Libraries/LibWebView/CMakeLists.txt', ' LibTextCodec LibThreading SDL3::SDL3)',
     ' LibTextCodec LibThreading)\nif (NOT TERMINALOS)\n    target_link_libraries(LibWebView PRIVATE SDL3::SDL3)\nendif()')

# The program's own file, as on Linux (the C library answers /proc/self/exe).
edit('Libraries/LibCore/System.cpp', '''    char path[4096] = {};
#if defined(AK_OS_LINUX) || defined(AK_OS_ANDROID)''', '''    char path[4096] = {};
#if defined(AK_OS_LINUX) || defined(AK_OS_ANDROID) || defined(AK_OS_TERMINALOS)''')

# Helper processes are this same program (UI/TerminalOS/main.cpp picks the
# service by argv[0]).
edit('Libraries/LibWebCommon/WebView/Utilities.cpp', '''    auto application_path = TRY(application_directory());
    Vector<ByteString> paths;
''', '''    Vector<ByteString> paths;
#if defined(AK_OS_TERMINALOS)
    (void)process_name;
    TRY(paths.try_append(TRY(Core::System::current_executable_path())));
    return paths;
#endif
    auto application_path = TRY(application_directory());
''')

# Fonts: Skia's empty font manager (Ladybird hands it the font files), from
# the share/fonts folder next to the program's.
edit('Libraries/LibGfx/Font/TypefaceSkia.cpp', '''#if defined(AK_OS_ANDROID)
#    include <ports/SkFontMgr_android.h>
#elif defined(AK_OS_WINDOWS)''', '''#if defined(AK_OS_ANDROID)
#    include <ports/SkFontMgr_android.h>
#elif defined(AK_OS_TERMINALOS)
#    include <ports/SkFontMgr_empty.h>
#elif defined(AK_OS_WINDOWS)''')
edit('Libraries/LibGfx/Font/TypefaceSkia.cpp', '''#if defined(AK_OS_ANDROID)
        font_manager = SkFontMgr_New_Android(nullptr);
#elif defined(AK_OS_WINDOWS)''', '''#if defined(AK_OS_ANDROID)
        font_manager = SkFontMgr_New_Android(nullptr);
#elif defined(AK_OS_TERMINALOS)
        font_manager = SkFontMgr_New_Custom_Empty();
#elif defined(AK_OS_WINDOWS)''')
edit('Libraries/LibGfx/Font/FontDatabase.cpp', '''#    elif defined(AK_OS_ANDROID)
    return Vector<String> { {''', '''#    elif defined(AK_OS_TERMINALOS)
    auto program = TRY(Core::System::current_executable_path());
    auto prefix = LexicalPath(LexicalPath(program).dirname()).dirname();
    return Vector<String> { {
        TRY(String::formatted("{}/share/fonts", prefix)),
    } };

#    elif defined(AK_OS_ANDROID)
    return Vector<String> { {''')

# The user interface: UI/TerminalOS (overlay/), not Qt.
edit('UI/CMakeLists.txt', '''if (ANDROID OR VCPKG_TARGET_ANDROID)
    add_subdirectory("Android")
else()''', '''if (ANDROID OR VCPKG_TARGET_ANDROID)
    add_subdirectory("Android")
elseif (TERMINALOS)
    add_subdirectory("TerminalOS")
else()''')

sys.exit(1 if failed else 0)
