//
// Copyright 2026 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// IOSurfaceSurfaceEAGL.mm: an IOSurface-backed pbuffer for the EAGL backend
//
// The CGL backend points an existing texture object at an IOSurface with
// CGLTexImageIOSurface2D. GLES has no such call; what it has is
// CVOpenGLESTextureCache, which makes a texture of its own out of the surface.
// So the surface creates that texture and hands its name over - to a framebuffer
// directly, or to the texture object that EGL_BindTexImage names, which takes
// the name for as long as the image is bound.

#import "libANGLE/renderer/gl/eagl/IOSurfaceSurfaceEAGL.h"

#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES2/glext.h>

#include <dlfcn.h>

#import "common/debug.h"
#import "libANGLE/AttributeMap.h"
#import "libANGLE/renderer/gl/BlitGL.h"
#import "libANGLE/renderer/gl/ContextGL.h"
#import "libANGLE/renderer/gl/FramebufferGL.h"
#import "libANGLE/renderer/gl/FunctionsGL.h"
#import "libANGLE/renderer/gl/RendererGL.h"
#import "libANGLE/renderer/gl/StateManagerGL.h"
#import "libANGLE/renderer/gl/renderergl_utils.h"

namespace
{

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
      mInternalFormat(GL_NONE),
      mAlphaInitialized(false),
      mPixelBuffer(nullptr),
      mTextureCache(nullptr),
      mSurfaceTexture(nullptr),
      mSurfaceTextureID(0),
      mFramebufferID(0)
{
    mIOSurface = reinterpret_cast<IOSurfaceRef>(buffer);
    CFRetain(mIOSurface);

    mWidth  = static_cast<int>(attribs.get(EGL_WIDTH));
    mHeight = static_cast<int>(attribs.get(EGL_HEIGHT));
    mPlane  = static_cast<int>(attribs.get(EGL_IOSURFACE_PLANE_ANGLE));

    mInternalFormat   = static_cast<GLenum>(attribs.get(EGL_TEXTURE_INTERNAL_FORMAT_ANGLE));
    mAlphaInitialized = !hasEmulatedAlphaChannel();
}

IOSurfaceSurfaceEAGL::~IOSurfaceSurfaceEAGL()
{
    if (mFramebufferID != 0)
    {
        mStateManager->deleteFramebuffer(mFramebufferID);
        mFramebufferID = 0;
    }
    if (mSurfaceTexture != nullptr)
    {
        CFRelease(mSurfaceTexture);
        mSurfaceTexture   = nullptr;
        mSurfaceTextureID = 0;
    }
    if (mTextureCache != nullptr)
    {
        CVOpenGLESTextureCacheFlush(mTextureCache, 0);
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

angle::Result IOSurfaceSurfaceEAGL::getBindTexImageTextureID(const gl::Context *context,
                                                             GLuint *textureIDOut)
{
    ANGLE_TRY(ensureSurfaceTexture(context));
    ANGLE_TRY(initializeAlphaChannel(context, mSurfaceTextureID));
    *textureIDOut = mSurfaceTextureID;
    return angle::Result::Continue;
}

angle::Result IOSurfaceSurfaceEAGL::initializeAlphaChannel(const gl::Context *context,
                                                           GLuint texture)
{
    if (mAlphaInitialized)
    {
        return angle::Result::Continue;
    }

    BlitGL *blitter = GetBlitGL(context);
    ANGLE_TRY(blitter->clearRenderableTextureAlphaToOne(context, texture,
                                                        gl::TextureTarget::_2D, 0));
    mAlphaInitialized = true;
    return angle::Result::Continue;
}

bool IOSurfaceSurfaceEAGL::hasEmulatedAlphaChannel() const
{
    return mInternalFormat == GL_RGB;
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

    return egl::NoError();
}

egl::Error IOSurfaceSurfaceEAGL::releaseTexImage(const gl::Context *context, EGLint buffer)
{
    GetFunctionsGL(context)->flush();

    if (mSurfaceTexture != nullptr && mFramebufferID == 0)
    {
        CFRelease(mSurfaceTexture);
        mSurfaceTexture   = nullptr;
        mSurfaceTextureID = 0;
        CVOpenGLESTextureCacheFlush(mTextureCache, 0);
    }

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

    EGLAttrib internalFormat = attribs.get(EGL_TEXTURE_INTERNAL_FORMAT_ANGLE);
    EGLAttrib type           = attribs.get(EGL_TEXTURE_TYPE_ANGLE);
    if ((internalFormat != GL_BGRA_EXT && internalFormat != GL_RGBA && internalFormat != GL_RGB) ||
        type != GL_UNSIGNED_BYTE)
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

    if (IsError(initializeAlphaChannel(context, mSurfaceTextureID)))
    {
        return egl::Error(EGL_CONTEXT_LOST, "Could not clear the IOSurface's alpha channel.");
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
