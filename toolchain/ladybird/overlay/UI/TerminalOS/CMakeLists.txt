# Ladybird on Terminal OS: one program, the browser and all its helper
# processes. Each helper's main.cpp is built in with its ladybird_main
# renamed; main.cpp picks one by the name it is run as (argv[0]), which is the
# name Ladybird gives a helper it starts.

set(helpers AudioServer Compositor ImageDecoder MediaServer RequestServer WebContent WebWorker)

set(SOURCES
    main.cpp
    Browser.cpp
    WebViewTOS.cpp
    WindowServer.cpp
    ${LADYBIRD_SOURCE_DIR}/Services/RendererSandboxUnimplemented.cpp
)

foreach(helper IN LISTS helpers)
    set(helper_main "${LADYBIRD_SOURCE_DIR}/Services/${helper}/main.cpp")
    set_source_files_properties("${helper_main}" PROPERTIES COMPILE_DEFINITIONS "ladybird_main=${helper}_main")
    list(APPEND SOURCES "${helper_main}")
    if (EXISTS "${LADYBIRD_SOURCE_DIR}/Services/${helper}/SandboxUnimplemented.cpp")
        list(APPEND SOURCES "${LADYBIRD_SOURCE_DIR}/Services/${helper}/SandboxUnimplemented.cpp")
    endif()
endforeach()
list(REMOVE_DUPLICATES SOURCES)

add_executable(ladybird ${SOURCES})
target_include_directories(ladybird PRIVATE ${LADYBIRD_SOURCE_DIR}/Services ${CMAKE_BINARY_DIR}/Services)
target_link_libraries(ladybird PRIVATE
    LibWebView LibWeb LibCore LibGfx LibIPC LibMain LibURL LibSandbox LibThreading LibFileSystem
    webcontentservice requestserverservice imagedecoderservice compositorservice
    audioserverservice mediaserverservice webworkerservice
    LibAudio LibMedia OpenSSL::Crypto OpenSSL::SSL skia ICU::uc)
set_target_properties(ladybird PROPERTIES OUTPUT_NAME "Ladybird")
create_ladybird_bundle(ladybird)
