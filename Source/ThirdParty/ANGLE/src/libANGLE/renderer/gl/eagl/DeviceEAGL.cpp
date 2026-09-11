//
// Copyright 2026 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// DeviceEAGL.cpp: EAGL implementation of egl::Device
//
// EAGL has no device object to hand out and no EGL_EAGL_* attribute to name it
// by, so this exists to satisfy the interface and answers nothing.

#include "libANGLE/renderer/gl/eagl/DeviceEAGL.h"

#include <EGL/eglext.h>

namespace rx
{

DeviceEAGL::DeviceEAGL() {}

DeviceEAGL::~DeviceEAGL() {}

egl::Error DeviceEAGL::initialize()
{
    return egl::NoError();
}

egl::Error DeviceEAGL::getAttribute(const egl::Display *display, EGLint attribute, void **outValue)
{
    return egl::Error(EGL_BAD_ATTRIBUTE);
}

void DeviceEAGL::generateExtensions(egl::DeviceExtensions *outExtensions) const {}

}  // namespace rx
