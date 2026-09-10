//
// Copyright 2026 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// IOSurfaceSurfaceEAGL.h: an IOSurface-backed pbuffer for the EAGL backend

#ifndef LIBANGLE_RENDERER_GL_EAGL_IOSURFACESURFACEEAGL_H_
#define LIBANGLE_RENDERER_GL_EAGL_IOSURFACESURFACEEAGL_H_

#include <CoreVideo/CoreVideo.h>

#include "libANGLE/renderer/gl/SurfaceGL.h"
#include "libANGLE/renderer/gl/eagl/DisplayEAGL.h"

struct __IOSurface;
typedef __IOSurface *IOSurfaceRef;

namespace egl
{
class AttributeMap;
}  // namespace egl

namespace rx
{

class DisplayEAGL;
class FunctionsGL;
class StateManagerGL;

class IOSurfaceSurfaceEAGL : public SurfaceGL
{
  public:
    IOSurfaceSurfaceEAGL(const egl::SurfaceState &state,
                         RendererGL *renderer,
                         EAGLContext *context,
                         EGLClientBuffer buffer,
                         const egl::AttributeMap &attribs);
    ~IOSurfaceSurfaceEAGL() override;

    egl::Error initialize(const egl::Display *display) override;
    egl::Error makeCurrent(const gl::Context *context) override;
    egl::Error unMakeCurrent(const gl::Context *context) override;

    egl::Error swap(const gl::Context *context, SurfaceSwapFeedback *feedback) override;
    egl::Error postSubBuffer(const gl::Context *context,
                             EGLint x,
                             EGLint y,
                             EGLint width,
                             EGLint height) override;
    egl::Error querySurfacePointerANGLE(EGLint attribute, void **value) override;
    egl::Error bindTexImage(const gl::Context *context,
                            gl::Texture *texture,
                            EGLint buffer) override;
    egl::Error releaseTexImage(const gl::Context *context, EGLint buffer) override;
    void setSwapInterval(const egl::Display *display, EGLint interval) override;

    gl::Extents getSize() const override;

    EGLint isPostSubBufferSupported() const override;
    EGLint getSwapBehavior() const override;

    static bool validateAttributes(EGLClientBuffer buffer, const egl::AttributeMap &attribs);

    egl::Error attachToFramebuffer(const gl::Context *context,
                                   gl::Framebuffer *framebuffer) override;
    egl::Error detachFromFramebuffer(const gl::Context *context,
                                     gl::Framebuffer *framebuffer) override;

  private:
    // The texture the IOSurface is visible through. GLES has no way to give an
    // existing texture object another texture's storage, which is what the CGL
    // backend does with CGLTexImageIOSurface2D, so the surface owns a texture of
    // its own and the two are copied where they have to meet.
    angle::Result ensureSurfaceTexture(const gl::Context *context);
    angle::Result copyBetweenTextures(const gl::Context *context,
                                      GLuint sourceTexture,
                                      GLuint destinationTexture);

    const FunctionsGL *mFunctions;
    StateManagerGL *mStateManager;

    EAGLContext *mEAGLContext;
    IOSurfaceRef mIOSurface;
    int mWidth;
    int mHeight;
    int mPlane;

    CVPixelBufferRef mPixelBuffer;
    CVOpenGLESTextureCacheRef mTextureCache;
    CVOpenGLESTextureRef mSurfaceTexture;
    GLuint mSurfaceTextureID;

    GLuint mBoundTextureID;
    GLuint mFramebufferID;
    GLuint mCopyFramebufferID;
};

}  // namespace rx

#endif  // LIBANGLE_RENDERER_GL_EAGL_IOSURFACESURFACEEAGL_H_
