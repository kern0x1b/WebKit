include(PlatformCocoa.cmake)

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

if (WEBKIT_IOS6)
    # Metal needs an A7 and an iOS 17 SDK; this device has neither, and the
    # renderer this port would use is the GLES one. Leaving the Metal backend in
    # the build only means compiling a renderer that can never be selected -
    # against SDK API that does not exist here.
    list(REMOVE_ITEM ANGLE_SOURCES ${metal_backend_sources} ${angle_translator_lib_msl_sources})
    list(REMOVE_ITEM ANGLE_DEFINITIONS ANGLE_ENABLE_METAL)

    # Until the GLES backend for this platform exists, the null renderer is what
    # keeps the library buildable: ANGLE refuses to compile with no renderer at
    # all. It draws nothing and must never be what a browser ships.
    list(APPEND ANGLE_SOURCES ${null_backend_sources})
    list(APPEND ANGLE_DEFINITIONS ANGLE_ENABLE_NULL)
endif ()
