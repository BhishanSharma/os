#!/usr/bin/env python3
# Ladybird for Terminal OS: the edits to Ladybird's own files. Run in the
# source tree (toolchain/ladybird/build.sh does, then copies overlay/ over it).
# Each edit replaces text that must occur exactly once; an edit already made
# is skipped, so running it twice is harmless.
import sys

failed = False


def edit(path, old, new, optional=False):
    global failed
    s = open(path, encoding='utf-8').read()
    if new in s or (optional and old not in s):
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

# ...and each helper starts with its own name as argv[0] (not the program's
# path, the same file for all of them), which is how main.cpp tells them apart.
edit('Libraries/LibCore/Process.cpp', '''    ArgvList argv_list(options.executable, options.arguments.size());''', '''#if defined(AK_OS_TERMINALOS)
    ArgvList argv_list(options.name.is_empty() ? options.executable : ByteString(options.name), options.arguments.size());
#else
    ArgvList argv_list(options.executable, options.arguments.size());
#endif''')

# (Diagnostics while fonts are brought up: what the font catalog found.)
edit('Libraries/LibWebView/FontService.cpp', '''            return callback_error.release_value();
    }

    auto serialized = TRY(builder->serialize());
    m_catalog_size = serialized.size();
    m_catalog_file = TRY(create_immutable_font_data(serialized.bytes()));
    return {};
}''', '''            return callback_error.release_value();
    }

    auto serialized = TRY(builder->serialize());
    m_catalog_size = serialized.size();
#if defined(AK_OS_TERMINALOS)
    warnln("FontService: {} directories ({}), {} faces, catalog {} bytes", directories.size(), directories.is_empty() ? String {} : directories.first(), next_face_id - 1, m_catalog_size);
#endif
    m_catalog_file = TRY(create_immutable_font_data(serialized.bytes()));
    return {};
}''')

# No HTTP/3 in Terminal OS's curl: a site's offer of it (Alt-Svc) is not taken
# up; the request goes over HTTP/2 or 1.1 as before.
edit('Services/RequestServer/Request.cpp', '''    if (m_alternative_service.has_value())
        set_option(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_3ONLY);''', '''#if !defined(AK_OS_TERMINALOS)
    if (m_alternative_service.has_value())
        set_option(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_3ONLY);
#endif''')

# (Diagnostics: which curl option a request could not set.)
edit('Services/RequestServer/Request.cpp', '''            dbgln("Request::handle_start_fetch_state: Failed to set curl option: {}", curl_easy_strerror(result));''',
     '''            dbgln("Request::handle_start_fetch_state: Failed to set curl option {}: {}", to_underlying(option), curl_easy_strerror(result));''')

# The stack's bounds, as on Linux (pthread_getattr_np): the JavaScript garbage
# collector scans the stack up to its top, and without them it ran past it.
edit('AK/StackInfo.cpp', '#elif defined(AK_OS_LINUX) || defined(AK_LIBC_GLIBC) || defined(AK_OS_MACOS) || defined(AK_OS_IOS) || defined(AK_OS_NETBSD) || defined(AK_OS_SOLARIS) || defined(AK_OS_HAIKU)\n#    include <pthread.h>',
     '#elif defined(AK_OS_LINUX) || defined(AK_LIBC_GLIBC) || defined(AK_OS_MACOS) || defined(AK_OS_IOS) || defined(AK_OS_NETBSD) || defined(AK_OS_SOLARIS) || defined(AK_OS_HAIKU) || defined(AK_OS_TERMINALOS)\n#    include <pthread.h>')
edit('AK/StackInfo.cpp', '#elif defined(AK_OS_LINUX) or defined(AK_LIBC_GLIBC) or defined(AK_OS_FREEBSD) or defined(AK_OS_NETBSD) or defined(AK_OS_SOLARIS) or defined(AK_OS_HAIKU)\n',
     '#elif defined(AK_OS_LINUX) or defined(AK_LIBC_GLIBC) or defined(AK_OS_FREEBSD) or defined(AK_OS_NETBSD) or defined(AK_OS_SOLARIS) or defined(AK_OS_HAIKU) or defined(AK_OS_TERMINALOS)\n')
edit('AK/StackInfo.cpp', '#    if defined(AK_OS_LINUX) or defined(AK_LIBC_GLIBC) or defined(AK_OS_HAIKU)\n',
     '#    if defined(AK_OS_LINUX) or defined(AK_LIBC_GLIBC) or defined(AK_OS_HAIKU) or defined(AK_OS_TERMINALOS)\n')

# Sound: AudioServer's Terminal OS backend (overlay: PlaybackStreamTerminalOS.cpp).
edit('Services/AudioServer/CMakeLists.txt', 'if (LADYBIRD_AUDIO_BACKEND STREQUAL "PULSE")\n',
     'if (TERMINALOS)\n    target_sources(audioserverservice PRIVATE PlaybackStreamTerminalOS.cpp)\n'
     '    target_compile_definitions(audioserverservice PRIVATE AUDIOSERVER_PLAYBACK_BACKEND=1)\n'
     'elseif (LADYBIRD_AUDIO_BACKEND STREQUAL "PULSE")\n')

# Thread IDs: gettid(), as on Linux (otherwise every thread is "none").
edit('AK/ThreadID.cpp', '''static u64 query_current_thread_id()
{
#if defined(AK_OS_LINUX)''', '''static u64 query_current_thread_id()
{
#if defined(AK_OS_LINUX) || defined(AK_OS_TERMINALOS)''')

# Anonymous shared memory: memfd_create, as on Linux (the C library has it).
edit('Libraries/LibCore/System.cpp', '''    int fd = -1;
#if defined(AK_OS_LINUX) || defined(AK_OS_FREEBSD)
    auto linux_options''', '''    int fd = -1;
#if defined(AK_OS_LINUX) || defined(AK_OS_FREEBSD) || defined(AK_OS_TERMINALOS)
    auto linux_options''')

# No SQL database yet: Terminal OS files open for reading or for writing, not
# both, which SQLite needs. History and cookies stay in memory until the browser quits.
edit('Libraries/LibWebView/Application.cpp', '''    bool disable_sql_database = false;
''', '''#if defined(AK_OS_TERMINALOS)
    bool disable_sql_database = true;
#else
    bool disable_sql_database = false;
#endif
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
edit('Libraries/LibGfx/Font/FontDatabase.cpp', '#include <LibGfx/Font/FontDatabase.h>\n',
     '#include <LibGfx/Font/FontDatabase.h>\n#include <AK/LexicalPath.h>\n#include <LibCore/System.h>\n')
# (An earlier version of the font folder edit below, already in a tree: the fixed one.)
edit('Libraries/LibGfx/Font/FontDatabase.cpp', '''    auto prefix = LexicalPath(LexicalPath(program).dirname()).dirname();
    return Vector<String> { {
        TRY(String::formatted("{}/share/fonts", prefix)),
    } };''', '''    auto prefix = LexicalPath(LexicalPath(program).dirname()).dirname();
    Vector<String> paths;
    paths.append(TRY(String::formatted("{}/share/fonts", prefix)));
    return paths;''', optional=True)

edit('Libraries/LibGfx/Font/FontDatabase.cpp', '''#    elif defined(AK_OS_ANDROID)
    return Vector<String> { {''', '''#    elif defined(AK_OS_TERMINALOS)
    // share/fonts beside the program's bin folder.
    auto program = TRY(Core::System::current_executable_path());
    auto prefix = LexicalPath(LexicalPath(program).dirname()).dirname();
    Vector<String> paths;
    paths.append(TRY(String::formatted("{}/share/fonts", prefix)));
    return paths;

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

# No gamepads (SDL3): UI/TerminalOS/GamepadManagerNone.cpp.
edit('Libraries/LibWebView/CMakeLists.txt', '    GamepadManager.cpp\n',
     '    $<IF:$<BOOL:${TERMINALOS}>,${LADYBIRD_SOURCE_DIR}/UI/TerminalOS/GamepadManagerNone.cpp,GamepadManager.cpp>\n')

# Random numbers: getentropy (the kernel's RDRAND), 256 bytes at a time.
edit('AK/Random.cpp', '''#elif defined(AK_OS_WINDOWS)
    // Documented to always return TRUE''', '''#elif defined(AK_OS_TERMINALOS)
    unsigned char* out = (unsigned char*)buf;
    while (size > 0u) {
        size_t chunk = size < 256 ? size : 256;
        if (getentropy(out, chunk) != 0) [[unlikely]]
            return Error::from_errno(errno);
        size -= chunk;
        out += chunk;
    }
#elif defined(AK_OS_WINDOWS)
    // Documented to always return TRUE''')
edit('AK/Random.cpp', '''#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#endif''', '''#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#endif

#if defined(AK_OS_TERMINALOS)
#    include <errno.h>
#    include <unistd.h>
#endif''')

# The User-Agent names Terminal OS, but keeps "X11; ... Linux x86_64": web
# servers only know the frozen tokens of major platforms, and send anything
# else degraded or broken pages. Ladybird's own pages (about:version) say
# Terminal OS.
edit('Libraries/LibWebCommon/Loader/UserAgent.h', '#elif defined(AK_OS_LINUX) || defined(AK_OS_TERMINALOS)\n#    define OS_STRING "Linux"',
     '#elif defined(AK_OS_LINUX)\n#    define OS_STRING "Linux"', optional=True)   # (what an earlier version made)
edit('Libraries/LibWebCommon/Loader/UserAgent.h', '#elif defined(AK_OS_LINUX)\n#    define OS_STRING "Linux"',
     '#elif defined(AK_OS_TERMINALOS)\n#    define OS_STRING "Terminal OS"\n#elif defined(AK_OS_LINUX)\n#    define OS_STRING "Linux"')
# On Terminal OS the browser is called Pointer (it is Ladybird, BSD-2-Clause).
edit('Libraries/LibWebCommon/Loader/UserAgent.h', '#define BROWSER_NAME "Ladybird"',
     '#if defined(AK_OS_TERMINALOS)\n#    define BROWSER_NAME "Pointer"\n#else\n#    define BROWSER_NAME "Ladybird"\n#endif')
edit('Libraries/LibWebCommon/Loader/UserAgent.h', '#else\n#    define UA_PLATFORM_STRING "X11; Linux x86_64"',
     '#elif defined(AK_OS_TERMINALOS)\n#    define UA_PLATFORM_STRING "X11; Terminal OS; Linux x86_64"\n#else\n#    define UA_PLATFORM_STRING "X11; Linux x86_64"')
edit('Libraries/LibWebCommon/Loader/UserAgent.h', '#    if defined(AK_OS_ANDROID) || defined(AK_OS_LINUX)\n#        define NAVIGATOR_PLATFORM_SYSTEM_NAME "Linux"',
     '#    if defined(AK_OS_ANDROID) || defined(AK_OS_LINUX) || defined(AK_OS_TERMINALOS)\n#        define NAVIGATOR_PLATFORM_SYSTEM_NAME "Linux"')

# Without compiled-code fault recovery the interpreter still names this (a
# branch it never takes there).
edit('Libraries/LibWasm/AbstractMachine/BytecodeInterpreter.cpp', '#if WASM_COMPILED_FAULT_RECOVERY_SUPPORTED\n',
     '#if !WASM_COMPILED_FAULT_RECOVERY_SUPPORTED\n[[maybe_unused]] static StringView cranelift_trap_message(u8) { return "Trap"sv; }\n#endif\n#if WASM_COMPILED_FAULT_RECOVERY_SUPPORTED\n')

# Rust crates are built as static archives for x86_64-unknown-linux-gnu (the
# OS's C library answers the calls they make). That is also the build
# machine's triple, so the OS's compiler must not become cargo's linker: it
# would link the build scripts, which run here. An archive needs no linker.
edit('Meta/CMake/rust_crate.cmake', '    if (NOT WIN32)\n        list(APPEND cargo_env\n            "CARGO_TARGET_${target_upper}_LINKER=${CMAKE_C_COMPILER}"',
     '    if (NOT WIN32 AND NOT TERMINALOS)\n        list(APPEND cargo_env\n            "CARGO_TARGET_${target_upper}_LINKER=${CMAKE_C_COMPILER}"')

# Crash backtraces: the return addresses up the frame pointers (Ladybird
# keeps them), to look up with addr2line in the unstripped program.
edit('AK/Assertions.cpp', '''#elif defined(AK_HAS_BACKTRACE_HEADER)
void dump_backtrace(unsigned frames_to_skip, [[maybe_unused]] unsigned max_depth)''', '''#elif defined(AK_OS_TERMINALOS)
void dump_backtrace(unsigned frames_to_skip, unsigned max_depth)
{
    auto** frame = static_cast<void**>(__builtin_frame_address(0));
    for (unsigned i = 0; frame && frame[1] && i < frames_to_skip + max_depth; ++i) {
        if (i >= frames_to_skip) {
            char line[32];
            snprintf(line, sizeof(line), "  at %p\\n", frame[1]);
            PRINT_ERROR(line);
        }
        auto** next = static_cast<void**>(frame[0]);
        if (next <= frame || reinterpret_cast<FlatPtr>(next) - reinterpret_cast<FlatPtr>(frame) > 0x100000)
            break;
        frame = next;
    }
}
#elif defined(AK_HAS_BACKTRACE_HEADER)
void dump_backtrace(unsigned frames_to_skip, [[maybe_unused]] unsigned max_depth)''')
edit('AK/Assertions.cpp', '#if defined(AK_OS_SERENITY)\n#    include <serenity.h>\n#endif', '#if defined(AK_OS_SERENITY)\n#    include <serenity.h>\n#endif\n\n#if defined(AK_OS_TERMINALOS)\n#    include <stdio.h>\n#endif', optional=True)

sys.exit(1 if failed else 0)
