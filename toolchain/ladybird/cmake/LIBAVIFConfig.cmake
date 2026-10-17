# libavif (built without its CMake package files): the target Ladybird links.
if (NOT TARGET avif)
    add_library(avif STATIC IMPORTED)
    set_target_properties(avif PROPERTIES
        IMPORTED_LOCATION "${CMAKE_SYSROOT}/usr/lib/libavif.a"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SYSROOT}/usr/include"
        INTERFACE_LINK_LIBRARIES "${CMAKE_SYSROOT}/usr/lib/libdav1d.a")
endif()
set(LIBAVIF_FOUND TRUE)
