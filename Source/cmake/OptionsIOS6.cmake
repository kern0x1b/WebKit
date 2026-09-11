# ImageIO decodes WebP from iOS 14, so this port builds the decoder WebKit
# carries for platforms that cannot. The image Accept header follows this: see
# acceptHeaderValueForImageResource in CachedResourceRequest.cpp.
if (USE_WEBP)
    find_package(WebP COMPONENTS demux)
    if (NOT WebP_FOUND)
        message(FATAL_ERROR "libwebp is required for USE_WEBP")
    endif ()
    SET_AND_EXPOSE_TO_BUILD(USE_WEBP ON)
endif ()

# Off by default because the system font parser handles WOFF2 from iOS 7 onward.
# It does not on the release this port targets, so the build turns it on and
# supplies the library; every other port that does this calls find_package here.
if (USE_WOFF2)
    find_package(WOFF2 1.0.2 COMPONENTS dec)
    if (NOT WOFF2_FOUND)
        message(FATAL_ERROR "libwoff2dec is required for USE_WOFF2")
    endif ()
endif ()



if (CMAKE_IOS_SIMULATOR OR CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(WEBKIT_PLATFORM_NAME "iPhoneSimulator")
    set(WEBKIT_SDK_NAME "iphonesimulator")
else ()
    set(WEBKIT_PLATFORM_NAME "iPhoneOS")
    set(WEBKIT_SDK_NAME "iphoneos")
endif ()
string(REGEX MATCH "^[0-9]+\\.[0-9]+" _sdk_major_minor "${_sdk_version}")
if (_sdk_major_minor AND (NOT CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS _sdk_major_minor))
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${_sdk_major_minor}" CACHE STRING "Minimum iOS version" FORCE)
    message(WARNING "Deployment target auto-set to SDK version: ${CMAKE_OSX_DEPLOYMENT_TARGET} (SPI header guards require this)")
endif ()

# Resolve the real clang once and pin it for the lifetime of this build tree.
# This is a build speed optimization, and also a defense against tearing between
# resolved toolchain and resolved SDK path / version.
WEBKIT_XCRUN(_clang -f clang)
if (EXISTS "${_clang}")
    set(CMAKE_C_COMPILER "${_clang}")
    set(CMAKE_CXX_COMPILER "${_clang}++")
    set(CMAKE_OBJC_COMPILER "${_clang}")
    set(CMAKE_OBJCXX_COMPILER "${_clang}++")
endif ()


enable_language(OBJC OBJCXX)

find_package(ZLIB REQUIRED)

# Strip ${SDK}/usr/include from ZLIB::ZLIB; reachable via -isysroot.
if (TARGET ZLIB::ZLIB)
    set_target_properties(ZLIB::ZLIB PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "")
endif ()

set(WebKit_LIBRARY_TYPE SHARED)

set(bmalloc_LIBRARY_TYPE OBJECT)
set(WTF_LIBRARY_TYPE OBJECT)
set(JavaScriptCore_LIBRARY_TYPE SHARED)
set(WebCore_LIBRARY_TYPE SHARED)

if (CMAKE_OSX_SYSROOT)
    add_link_options("-F${CMAKE_OSX_SYSROOT}/System/Library/Frameworks")
    add_link_options("-F${CMAKE_OSX_SYSROOT}/System/Library/PrivateFrameworks")
    add_compile_options("$<$<COMPILE_LANGUAGE:Swift>:SHELL:-Fsystem ${CMAKE_OSX_SYSROOT}/System/Library/PrivateFrameworks>")
    set(WEBKIT_PRIVATE_FRAMEWORKS_COMPILE_FLAG "$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-iframework${CMAKE_OSX_SYSROOT}/System/Library/PrivateFrameworks>")
endif ()

if (CMAKE_OSX_SYSROOT MATCHES "\\.Internal\\.sdk$")
    add_compile_options("$<$<COMPILE_LANGUAGE:Swift>:-DUSE_APPLE_INTERNAL_SDK>")
    add_compile_options("$<$<COMPILE_LANGUAGE:Swift>:SHELL:-Xcc -DUSE_APPLE_INTERNAL_SDK>")
endif ()

# VFS overlay: suppress TextInput_Private which uses ICU types without a
# proper module dependency.  The umbrella header drags in TI_NSStringExtras.h
# whose UChar references fail during explicit-module builds of Swift targets.
set(_textinput_private "${CMAKE_OSX_SYSROOT}/System/Library/PrivateFrameworks/TextInput.framework/Modules/module.private.modulemap")
if (EXISTS "${_textinput_private}")
    set(_empty_modulemap "${CMAKE_BINARY_DIR}/empty-module.private.modulemap")
    file(WRITE "${_empty_modulemap}" "")
    set(_vfs_overlay "${CMAKE_BINARY_DIR}/ios-swift-vfs-overlay.yaml")
    file(WRITE "${_vfs_overlay}"
"{
  \"version\": 0,
  \"case-sensitive\": false,
  \"roots\": [
    {
      \"name\": \"${_textinput_private}\",
      \"type\": \"file\",
      \"external-contents\": \"${_empty_modulemap}\"
    }
  ]
}
")
    add_compile_options("$<$<COMPILE_LANGUAGE:Swift>:SHELL:-Xcc -ivfsoverlay -Xcc ${_vfs_overlay}>")
endif ()

add_compile_options(
    "$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-Wno-shorten-64-to-32>"
    "$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-Wno-sign-conversion>"
    "$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-Wno-conversion>"
    "$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-Wno-float-conversion>"
    "$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-Wno-shadow>"
    "$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-Wno-overloaded-virtual>"
    "$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-Wno-reserved-identifier>"
    "$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-Wno-exit-time-destructors>"
    "$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-Wno-implicit-fallthrough>"
)

add_compile_options("$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-Wno-error=#warnings>")
add_compile_options("$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-Wno-objc-method-access>")

add_compile_options(
    "$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-fno-common>"
)

if (ENABLE_SANITIZERS)
    add_compile_definitions(ENABLE_CONJECTURE_ASSERT=1)
endif ()

set(IOS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}" CACHE STRING "" FORCE)
if (CMAKE_IOS_SIMULATOR OR CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(PLATFORM_NAME "iPhoneSimulator" CACHE STRING "" FORCE)
else ()
    set(PLATFORM_NAME "iPhoneOS" CACHE STRING "" FORCE)
endif ()

set(CMAKE_BUILD_WITH_INSTALL_NAME_DIR ON)
set(JavaScriptCore_INSTALL_NAME_DIR "/System/Library/Frameworks" CACHE STRING "" FORCE)
set(WebKit_INSTALL_NAME_DIR "/System/Library/Frameworks" CACHE STRING "" FORCE)
set(WebCore_INSTALL_NAME_DIR "/System/Library/PrivateFrameworks" CACHE STRING "" FORCE)
set(WebGPU_INSTALL_NAME_DIR "/System/Library/PrivateFrameworks" CACHE STRING "" FORCE)
set(WebKitLegacy_INSTALL_NAME_DIR "/System/Library/PrivateFrameworks" CACHE STRING "" FORCE)

if (WEBKIT_ADDITIONS_INCLUDE_PATH AND EXISTS "${WEBKIT_ADDITIONS_INCLUDE_PATH}/WebKitAdditions/CMake/OptionsIOS.cmake")
    message(STATUS "WebKitAdditions CMake: ${WEBKIT_ADDITIONS_INCLUDE_PATH}/WebKitAdditions/CMake/OptionsIOS.cmake")
    include("${WEBKIT_ADDITIONS_INCLUDE_PATH}/WebKitAdditions/CMake/OptionsIOS.cmake")
endif ()

if (CMAKE_OSX_SYSROOT AND EXISTS "${CMAKE_OSX_SYSROOT}/usr/local/include")
    add_compile_options("$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-isystem${CMAKE_OSX_SYSROOT}/usr/local/include>")
    add_compile_options("$<$<COMPILE_LANGUAGE:Swift>:SHELL:-Xcc -isystem${CMAKE_OSX_SYSROOT}/usr/local/include>")
endif ()

