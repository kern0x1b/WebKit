//
// Copyright 2026 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// IOSurfaceSurfaceEAGL.mm: an IOSurface-backed pbuffer for the EAGL backend
//
// The CGL backend hands an IOSurface to an existing texture object with
// CGLTexImageIOSurface2D. GLES has no such call and no rectangle textures; what
// it has is CVOpenGLESTextureCache, which produces a texture of its own that is
// backed by the surface. So this surface owns that texture, gives it directly to
// a framebuffer when it can, and copies to and from the caller's texture when
// EGL_BindTexImage is used instead.

#import "libANGLE/renderer/gl/eagl/IOSurfaceSurfaceEAGL.h"

#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES2/glext.h>

#include <dlfcn.h>

#import "common/debug.h"
#import "libANGLE/AttributeMap.h"
#import "libANGLE/renderer/gl/ContextGL.h"
#import "libANGLE/renderer/gl/FramebufferGL.h"
#import "libANGLE/renderer/gl/FunctionsGL.h"
#import "libANGLE/renderer/gl/RendererGL.h"
#import "libANGLE/renderer/gl/StateManagerGL.h"
#import "libANGLE/renderer/gl/TextureGL.h"
#import "libANGLE/renderer/gl/renderergl_utils.h"

namespace
{

// IOSurface is a private framework on this release, so the three calls this
// file needs are resolved at runtime rather than linked - the same way the rest
// of the port reaches it.
void *iosurfaceLibrary()
{
    static void *library =
        dlopen("/System/Library/PrivateFrameworks/IOSurface.framework/IOSurface", RTLD_LAZY);
    return library;
}

size_t surfacePlaneCount(IOSurfaceRef surface)
{
    typedef size_t (*PlaneCountFunction)(IOSurfaceRef);
    PlaneCountFunction planeCount =
        reinterpret_cast<PlaneCountFunction>(dlsym(iosurfaceLibrary(), "IOSurfaceGetPlaneCount"));
    return planeCount ? planeCount(surface) : 0;
}

size_t surfaceWidthOfPlane(IOSurfaceRef surface, size_t plane)
{
    typedef size_t (*WidthFunction)(IOSurfaceRef, size_t);
    WidthFunction width =
        reinterpret_cast<WidthFunction>(dlsym(iosurfaceLibrary(), "IOSurfaceGetWidthOfPlane"));
    return width ? width(surface, plane) : 0;
}

size_t surfaceHeightOfPlane(IOSurfaceRef surface, size_t plane)
{
    typedef size_t (*HeightFunction)(IOSurfaceRef, size_t);
    HeightFunction height =
        reinterpret_cast<HeightFunction>(dlsym(iosurfaceLibrary(), "IOSurfaceGetHeightOfPlane"));
    return height ? height(surface, plane) : 0;
}

}  // anonymous namespace

namespace rx
{

IOSurfaceSurfaceEAGL::IOSurfaceSurfaceEAGL(const egl::SurfaceState &state,
                                           RendererGL *renderer,
                                           EAGLContext *context,
                                           EGLClientBuffer buffer,
                                           const egl::AttributeMap &attribs)
    : SurfaceGL(state),
      mFunctions(renderer->getFunctions()),
      mStateManager(renderer->getStateManager()),
      mEAGLContext(context),
      mIOSurface(nullptr),
      mWidth(0),
      mHeight(0),
      mPlane(0),
      mPixelBuffer(nullptr),
      mTextureCache(nullptr),
      mSurfaceTexture(nullptr),
      mSurfaceTextureID(0),
      mBoundTextureID(0),
      mFramebufferID(0),
      mCopyFramebufferID(0)
{
    // Keep a reference so the surface outlives whoever handed it over.
    mIOSurface = reinterpret_cast<IOSurfaceRef>(buffer);
    CFRetain(mIOSurface);

    mWidth  = static_cast<int>(attribs.get(EGL_WIDTH));
    mHeight = static_cast<int>(attribs.get(EGL_HEIGHT));
    mPlane  = static_cast<int>(attribs.get(EGL_IOSURFACE_PLANE_ANGLE));
}

IOSurfaceSurfaceEAGL::~IOSurfaceSurfaceEAGL()
{
    if (mFramebufferID != 0)
    {
        mStateManager->deleteFramebuffer(mFramebufferID);
        mFramebufferID = 0;
    }
    if (mCopyFramebufferID != 0)
    {
        mStateManager->deleteFramebuffer(mCopyFramebufferID);
        mCopyFramebufferID = 0;
    }
    if (mSurfaceTexture != nullptr)
    {
        CFRelease(mSurfaceTexture);
        mSurfaceTexture   = nullptr;
        mSurfaceTextureID = 0;
    }
    if (mTextureCache != nullptr)
    {
        CFRelease(mTextureCache);
        mTextureCache = nullptr;
    }
    if (mPixelBuffer != nullptr)
    {
        CFRelease(mPixelBuffer);
        mPixelBuffer = nullptr;
    }
    if (mIOSurface != nullptr)
    {
        CFRelease(mIOSurface);
        mIOSurface = nullptr;
    }
}

egl::Error IOSurfaceSurfaceEAGL::initialize(const egl::Display *display)
{
    if (CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, mIOSurface, nullptr, &mPixelBuffer) !=
        kCVReturnSuccess)
    {
        return egl::Error(EGL_BAD_ALLOC, "Could not wrap the IOSurface in a CVPixelBuffer.");
    }

    if (CVOpenGLESTextureCacheCreate(kCFAllocatorDefault, nullptr, mEAGLContext, nullptr,
                                     &mTextureCache) != kCVReturnSuccess)
    {
        return egl::Error(EGL_BAD_ALLOC, "Could not create the texture cache for the IOSurface.");
    }

    return egl::NoError();
}

angle::Result IOSurfaceSurfaceEAGL::ensureSurfaceTexture(const gl::Context *context)
{
    if (mSurfaceTextureID != 0)
    {
        return angle::Result::Continue;
    }

    // BGRA is the order an IOSurface arrives in, and the order this GPU's
    // texture upload wants to be told about; the internal format stays RGBA.
    CVReturn result = CVOpenGLESTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, mTextureCache, mPixelBuffer, nullptr, GL_TEXTURE_2D, GL_RGBA, mWidth,
        mHeight, GL_BGRA_EXT, GL_UNSIGNED_BYTE, mPlane, &mSurfaceTexture);
    ANGLE_CHECK(GetImplAs<ContextGL>(context), result == kCVReturnSuccess,
                "Could not make a texture from the IOSurface.", GL_OUT_OF_MEMORY);

    mSurfaceTextureID = CVOpenGLESTextureGetName(mSurfaceTexture);
    mStateManager->bindTexture(gl::TextureType::_2D, mSurfaceTextureID);
    mFunctions->texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    mFunctions->texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    mFunctions->texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    mFunctions->texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    return angle::Result::Continue;
}

angle::Result IOSurfaceSurfaceEAGL::copyBetweenTextures(const gl::Context *context,
                                                        GLuint sourceTexture,
                                                        GLuint destinationTexture)
{
    if (mCopyFramebufferID == 0)
    {
        mFunctions->genFramebuffers(1, &mCopyFramebufferID);
    }

    mStateManager->bindFramebuffer(GL_READ_FRAMEBUFFER, mCopyFramebufferID);
    mFunctions->framebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                     sourceTexture, 0);

    mStateManager->bindTexture(gl::TextureType::_2D, destinationTexture);
    mFunctions->copyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, mWidth, mHeight);

    return angle::Result::Continue;
}

egl::Error IOSurfaceSurfaceEAGL::makeCurrent(const gl::Context *context)
{
    return egl::NoError();
}

egl::Error IOSurfaceSurfaceEAGL::unMakeCurrent(const gl::Context *context)
{
    GetFunctionsGL(context)->flush();
    return egl::NoError();
}

egl::Error IOSurfaceSurfaceEAGL::swap(const gl::Context *context, SurfaceSwapFeedback *feedback)
{
    return egl::NoError();
}

egl::Error IOSurfaceSurfaceEAGL::postSubBuffer(const gl::Context *context,
                                               EGLint x,
                                               EGLint y,
                                               EGLint width,
                                               EGLint height)
{
    UNREACHABLE();
    return egl::NoError();
}

egl::Error IOSurfaceSurfaceEAGL::querySurfacePointerANGLE(EGLint attribute, void **value)
{
    UNREACHABLE();
    return egl::NoError();
}

egl::Error IOSurfaceSurfaceEAGL::bindTexImage(const gl::Context *context,
                                              gl::Texture *texture,
                                              EGLint buffer)
{
    if (IsError(ensureSurfaceTexture(context)))
    {
        return egl::Error(EGL_CONTEXT_LOST, "Could not make a texture from the IOSurface.");
    }

    const TextureGL *textureGL = GetImplAs<TextureGL>(texture);
    mBoundTextureID            = textureGL->getTextureID();

    // Give the caller's texture storage of its own and start it from what the
    // surface currently holds, so a page that draws over part of the canvas
    // does not lose the rest of it.
    mStateManager->bindTexture(gl::TextureType::_2D, mBoundTextureID);
    mFunctions->texImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mWidth, mHeight, 0, GL_RGBA,
                           GL_UNSIGNED_BYTE, nullptr);
    if (IsError(copyBetweenTextures(context, mSurfaceTextureID, mBoundTextureID)))
    {
        return egl::Error(EGL_CONTEXT_LOST, "Could not read the IOSurface into the texture.");
    }

    return egl::NoError();
}

egl::Error IOSurfaceSurfaceEAGL::releaseTexImage(const gl::Context *context, EGLint buffer)
{
    if (mBoundTextureID != 0 && mSurfaceTextureID != 0)
    {
        if (IsError(copyBetweenTextures(context, mBoundTextureID, mSurfaceTextureID)))
        {
            return egl::Error(EGL_CONTEXT_LOST, "Could not write the texture into the IOSurface.");
        }
        mBoundTextureID = 0;
    }

    const FunctionsGL *functions = GetFunctionsGL(context);
    functions->flush();
    return egl::NoError();
}

void IOSurfaceSurfaceEAGL::setSwapInterval(const egl::Display *display, EGLint interval)
{
    UNREACHABLE();
}

gl::Extents IOSurfaceSurfaceEAGL::getSize() const
{
    return gl::Extents(mWidth, mHeight, 1);
}

EGLint IOSurfaceSurfaceEAGL::isPostSubBufferSupported() const
{
    UNREACHABLE();
    return EGL_FALSE;
}

EGLint IOSurfaceSurfaceEAGL::getSwapBehavior() const
{
    return EGL_BUFFER_PRESERVED;
}

// static
bool IOSurfaceSurfaceEAGL::validateAttributes(EGLClientBuffer buffer,
                                              const egl::AttributeMap &attribs)
{
    IOSurfaceRef ioSurface = reinterpret_cast<IOSurfaceRef>(buffer);

    size_t planeCount = std::max(size_t(1), surfacePlaneCount(ioSurface));
    EGLAttrib plane   = attribs.get(EGL_IOSURFACE_PLANE_ANGLE);
    if (plane < 0 || static_cast<size_t>(plane) >= planeCount)
    {
        return false;
    }

    EGLAttrib width  = attribs.get(EGL_WIDTH);
    EGLAttrib height = attribs.get(EGL_HEIGHT);
    if (width <= 0 || height <= 0 ||
        width > static_cast<EGLAttrib>(surfaceWidthOfPlane(ioSurface, plane)) ||
        height > static_cast<EGLAttrib>(surfaceHeightOfPlane(ioSurface, plane)))
    {
        return false;
    }

    // Only the eight-bit four-channel format is served here. It is what a canvas
    // is, and claiming the rest would mean claiming conversions this backend
    // does not do.
    EGLAttrib internalFormat = attribs.get(EGL_TEXTURE_INTERNAL_FORMAT_ANGLE);
    EGLAttrib type           = attribs.get(EGL_TEXTURE_TYPE_ANGLE);
    if ((internalFormat != GL_BGRA_EXT && internalFormat != GL_RGBA) || type != GL_UNSIGNED_BYTE)
    {
        return false;
    }

    return true;
}

egl::Error IOSurfaceSurfaceEAGL::attachToFramebuffer(const gl::Context *context,
                                                     gl::Framebuffer *framebuffer)
{
    FramebufferGL *framebufferGL = GetImplAs<FramebufferGL>(framebuffer);
    ASSERT(framebufferGL->getFramebufferID() == 0);

    if (IsError(ensureSurfaceTexture(context)))
    {
        return egl::Error(EGL_CONTEXT_LOST, "Could not make a texture from the IOSurface.");
    }

    if (mFramebufferID == 0)
    {
        GLuint framebufferID = 0;
        mFunctions->genFramebuffers(1, &framebufferID);
        mStateManager->bindFramebuffer(GL_FRAMEBUFFER, framebufferID);
        mFunctions->framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                         mSurfaceTextureID, 0);
        mFramebufferID = framebufferID;
    }

    framebufferGL->setFramebufferID(mFramebufferID);
    return egl::NoError();
}

egl::Error IOSurfaceSurfaceEAGL::detachFromFramebuffer(const gl::Context *context,
                                                       gl::Framebuffer *framebuffer)
{
    FramebufferGL *framebufferGL = GetImplAs<FramebufferGL>(framebuffer);
    ASSERT(framebufferGL->getFramebufferID() == mFramebufferID);

    framebufferGL->setFramebufferID(0);
    return egl::NoError();
}

}  // namespace rx
