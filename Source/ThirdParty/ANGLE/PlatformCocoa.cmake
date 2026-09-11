find_library(COREGRAPHICS_LIBRARY CoreGraphics)
if (NOT COREGRAPHICS_LIBRARY)  # ios6: allow missing frameworks
    set(COREGRAPHICS_LIBRARY "")
endif ()
find_library(FOUNDATION_LIBRARY Foundation)
if (NOT FOUNDATION_LIBRARY)  # ios6: allow missing frameworks
    set(FOUNDATION_LIBRARY "")
endif ()
find_library(IOSURFACE_LIBRARY IOSurface)
if (NOT IOSURFACE_LIBRARY)  # ios6: allow missing frameworks
    set(IOSURFACE_LIBRARY "")
endif ()
if (NOT IOSURFACE_LIBRARY)
    set(IOSURFACE_LIBRARY "")
endif ()
find_library(METAL_LIBRARY Metal)
if (NOT METAL_LIBRARY)  # ios6: allow missing frameworks
    set(METAL_LIBRARY "")
endif ()
if (NOT TARGET ZLIB::ZLIB)
    find_package(ZLIB REQUIRED)
endif ()

list(APPEND ANGLE_SOURCES
    ${metal_backend_sources}

    ${angle_translator_lib_msl_sources}

    ${libangle_mac_sources}
    ${libangle_gpu_info_util_sources}
)

list(APPEND ANGLE_DEFINITIONS
    ANGLE_ENABLE_METAL
)

list(APPEND ANGLEGLESv2_LIBRARIES
    ${COREGRAPHICS_LIBRARY}
    ${FOUNDATION_LIBRARY}
    ${IOSURFACE_LIBRARY}
    ${METAL_LIBRARY}
)


if (WEBKIT_SDK_IS_MACOS)
    find_library(IOKIT_LIBRARY IOKit)
    find_library(QUARTZ_LIBRARY Quartz)

    list(APPEND ANGLE_SOURCES
        ${libangle_gpu_info_util_mac_sources}
    )

    list(APPEND ANGLEGLESv2_LIBRARIES
        ${IOKIT_LIBRARY}
        ${QUARTZ_LIBRARY}
    )
else ()
    find_library(QUARTZCORE_LIBRARY QuartzCore)

    list(REMOVE_ITEM ANGLE_SOURCES
        src/common/gl/cgl/FunctionsCGL.cpp
        src/common/gl/cgl/FunctionsCGL.h
        src/common/system_utils_mac.cpp
    )

    list(APPEND ANGLE_SOURCES
        ${libangle_gpu_info_util_ios_sources}
        src/libANGLE/renderer/driver_utils_ios.mm
    )

    list(APPEND ANGLEGLESv2_LIBRARIES
        ${QUARTZCORE_LIBRARY}
    )
endif ()
