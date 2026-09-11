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

if (WEBKIT_SDK_IS_IOS_FAMILY)

    find_library(QUARTZCORE_LIBRARY QuartzCore)

    list(REMOVE_ITEM ANGLE_SOURCES
        src/common/gl/cgl/FunctionsCGL.cpp
        src/common/gl/cgl/FunctionsCGL.h
        src/common/system_utils_mac.cpp
    )

    # The mac file above is removed and nothing replaced it, so the library was
    # short one definition - GetSharedLibraryExtension - which only shows up when
    # something links against it.
    list(APPEND ANGLE_SOURCES
        src/common/system_utils_ios.cpp
    )

    list(APPEND ANGLE_SOURCES
        ${libangle_gpu_info_util_ios_sources}
        src/libANGLE/renderer/driver_utils_ios.mm
    )

    list(APPEND ANGLEGLESv2_LIBRARIES
        ${QUARTZCORE_LIBRARY}
    )

    if (WEBKIT_IOS6)
        # Metal needs an A7 and an iOS 17 SDK; this device has neither, and the
        # renderer this port would use is the GLES one. Leaving the Metal backend in
        # the build only means compiling a renderer that can never be selected -
        # against SDK API that does not exist here.
        list(REMOVE_ITEM ANGLE_SOURCES ${metal_backend_sources} ${angle_translator_lib_msl_sources})
        list(REMOVE_ITEM ANGLE_DEFINITIONS ANGLE_ENABLE_METAL)

        # The renderer this hardware can actually run: GLES through EAGL.
        list(APPEND ANGLE_SOURCES ${gl_backend_sources})
        list(APPEND ANGLE_DEFINITIONS ANGLE_ENABLE_OPENGL ANGLE_ENABLE_EAGL)

        find_library(OPENGLES_FRAMEWORK OpenGLES)
        list(APPEND ANGLEGLESv2_LIBRARIES ${OPENGLES_FRAMEWORK})
    endif ()
endif ()
