//
// Copyright 2026 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// DisplayEAGL.mm: EAGL implementation of egl::Display
//
// The GLES counterpart of DisplayCGL, for hardware that predates Metal. EAGL
// has no pixel formats and no explicit device: a context is created for an API
// version and made current on a thread, and that is the whole surface of it.

#import "libANGLE/renderer/gl/eagl/DisplayEAGL.h"

#import <OpenGLES/EAGL.h>
#import <dlfcn.h>

#import "common/debug.h"
#import "common/system_utils.h"
#import "libANGLE/Display.h"
#import "libANGLE/Error.h"
#import "libANGLE/renderer/gl/ContextGL.h"
#import "libANGLE/renderer/gl/RendererGL.h"
#import "libANGLE/renderer/gl/eagl/DeviceEAGL.h"
#import "libANGLE/renderer/gl/eagl/IOSurfaceSurfaceEAGL.h"
#import "libANGLE/renderer/gl/eagl/PbufferSurfaceEAGL.h"

namespace
{

const char *kOpenGLESFrameworkName = "/System/Library/Frameworks/OpenGLES.framework/OpenGLES";

}  // namespace

namespace rx
{

class FunctionsGLEAGL : public FunctionsGL
{
  public:
    FunctionsGLEAGL(void *dylibHandle) : mDylibHandle(dylibHandle) {}

    ~FunctionsGLEAGL() override { dlclose(mDylibHandle); }

  private:
    void *loadProcAddress(const std::string &function) const override
    {
        return dlsym(mDylibHandle, function.c_str());
    }

    void *mDylibHandle;
};

DisplayEAGL::DisplayEAGL(const egl::DisplayState &state)
    : DisplayGL(state), mEGLDisplay(nullptr), mContext(nullptr), mThreadsWithCurrentContext()
{}

DisplayEAGL::~DisplayEAGL() {}

egl::Error DisplayEAGL::initialize(egl::Display *display)
{
    mEGLDisplay = display;

    mContext = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
    if (mContext == nullptr)
    {
        return egl::Error(EGL_NOT_INITIALIZED, "Could not create the EAGL context.");
    }

    if (![EAGLContext setCurrentContext:mContext])
    {
        return egl::Error(EGL_NOT_INITIALIZED, "Could not make the EAGL context current.");
    }
    mThreadsWithCurrentContext.insert(angle::GetCurrentThreadUniqueId());

    // EAGL has no getProcAddress, so the framework is opened directly, the way
    // the CGL backend opens libGL.
    void *handle = dlopen(kOpenGLESFrameworkName, RTLD_NOW);
    if (!handle)
    {
        return egl::Error(EGL_NOT_INITIALIZED, "Could not open the OpenGLES framework.");
    }

    std::unique_ptr<FunctionsGL> functionsGL(new FunctionsGLEAGL(handle));
    functionsGL->initialize(display->getAttributeMap());

    mRenderer.reset(new RendererGL(std::move(functionsGL), display->getAttributeMap(), this));

    const gl::Version &maxVersion = mRenderer->getMaxSupportedESVersion();
    if (maxVersion < gl::Version(2, 0))
    {
        return egl::Error(EGL_NOT_INITIALIZED, "OpenGL ES 2.0 is not supportable.");
    }

    return DisplayGL::initialize(display);
}

void DisplayEAGL::terminate()
{
    DisplayGL::terminate();

    mRenderer.reset();
    if (mContext != nullptr)
    {
        [EAGLContext setCurrentContext:nullptr];
        [mContext release];
        mContext = nullptr;
        mThreadsWithCurrentContext.clear();
    }
}

egl::Error DisplayEAGL::prepareForCall()
{
    if (!mContext)
    {
        return egl::Error(EGL_NOT_INITIALIZED, "Context not allocated.");
    }
    auto threadId = angle::GetCurrentThreadUniqueId();
    if (mThreadsWithCurrentContext.find(threadId) == mThreadsWithCurrentContext.end())
    {
        if (![EAGLContext setCurrentContext:mContext])
        {
            return egl::Error(EGL_BAD_ALLOC, "Could not make device EAGL context current.");
        }
        mThreadsWithCurrentContext.insert(threadId);
    }
    return egl::NoError();
}

egl::Error DisplayEAGL::releaseThread()
{
    ASSERT(mContext);
    auto threadId = angle::GetCurrentThreadUniqueId();
    if (mThreadsWithCurrentContext.find(threadId) != mThreadsWithCurrentContext.end())
    {
        if (![EAGLContext setCurrentContext:nullptr])
        {
            return egl::Error(EGL_BAD_ALLOC, "Could not release device EAGL context.");
        }
        mThreadsWithCurrentContext.erase(threadId);
    }
    return egl::NoError();
}

egl::Error DisplayEAGL::makeCurrent(egl::Display *display,
                                    egl::Surface *drawSurface,
                                    egl::Surface *readSurface,
                                    gl::Context *context)
{
    return DisplayGL::makeCurrent(display, drawSurface, readSurface, context);
}

SurfaceImpl *DisplayEAGL::createWindowSurface(const egl::SurfaceState &state,
                                              EGLNativeWindowType window,
                                              const egl::AttributeMap &attribs)
{
    // A window here is a CAEAGLLayer, which nothing in this port draws to: the
    // browser composites through an IOSurface.
    UNIMPLEMENTED();
    return nullptr;
}

SurfaceImpl *DisplayEAGL::createPbufferSurface(const egl::SurfaceState &state,
                                               const egl::AttributeMap &attribs)
{
    EGLint width  = static_cast<EGLint>(attribs.get(EGL_WIDTH, 0));
    EGLint height = static_cast<EGLint>(attribs.get(EGL_HEIGHT, 0));
    return new PbufferSurfaceEAGL(state, mRenderer.get(), width, height);
}

SurfaceImpl *DisplayEAGL::createPbufferFromClientBuffer(const egl::SurfaceState &state,
                                                        EGLenum buftype,
                                                        EGLClientBuffer clientBuffer,
                                                        const egl::AttributeMap &attribs)
{
    ASSERT(buftype == EGL_IOSURFACE_ANGLE);

    return new IOSurfaceSurfaceEAGL(state, mRenderer.get(), mContext, clientBuffer, attribs);
}

egl::Error DisplayEAGL::validateClientBuffer(const egl::Config *configuration,
                                             EGLenum buftype,
                                             EGLClientBuffer clientBuffer,
                                             const egl::AttributeMap &attribs) const
{
    ASSERT(buftype == EGL_IOSURFACE_ANGLE);

    if (!IOSurfaceSurfaceEAGL::validateAttributes(clientBuffer, attribs))
    {
        return egl::Error(EGL_BAD_ATTRIBUTE);
    }

    return egl::NoError();
}

SurfaceImpl *DisplayEAGL::createPixmapSurface(const egl::SurfaceState &state,
                                              NativePixmapType nativePixmap,
                                              const egl::AttributeMap &attribs)
{
    UNIMPLEMENTED();
    return nullptr;
}

ContextImpl *DisplayEAGL::createContext(const gl::State &state,
                                        gl::ErrorSet *errorSet,
                                        const egl::Config *configuration,
                                        const gl::Context *shareContext,
                                        const egl::AttributeMap &attribs)
{
    return new ContextGL(state, errorSet, mRenderer,
                         RobustnessVideoMemoryPurgeStatus::NOT_REQUESTED);
}

DeviceImpl *DisplayEAGL::createDevice()
{
    return new DeviceEAGL();
}

egl::ConfigSet DisplayEAGL::generateConfigs()
{
    egl::ConfigSet configs;

    const gl::Version &maxVersion = getMaxSupportedESVersion();
    ASSERT(maxVersion >= gl::Version(2, 0));
    bool supportsES3 = maxVersion >= gl::Version(3, 0);

    egl::Config config;

    config.nativeVisualID   = 0;
    config.nativeVisualType = 0;
    config.nativeRenderable = EGL_TRUE;

    config.redSize     = 8;
    config.greenSize   = 8;
    config.blueSize    = 8;
    config.alphaSize   = 8;
    config.depthSize   = 24;
    config.stencilSize = 8;

    config.colorBufferType = EGL_RGB_BUFFER;
    config.luminanceSize   = 0;
    config.alphaMaskSize   = 0;

    config.bufferSize = config.redSize + config.greenSize + config.blueSize + config.alphaSize;

    config.transparentType = EGL_NONE;

    config.maxPBufferWidth  = 2048;
    config.maxPBufferHeight = 2048;
    config.maxPBufferPixels = 2048 * 2048;

    config.configCaveat = EGL_NONE;

    config.sampleBuffers     = 0;
    config.samples           = 0;
    config.level             = 0;
    config.bindToTextureRGB  = EGL_FALSE;
    config.bindToTextureRGBA = EGL_FALSE;

    // GLES has no rectangle textures; an IOSurface reaches a shader as an
    // ordinary 2D texture here.
    config.bindToTextureTarget = EGL_TEXTURE_2D;

    // eglChooseConfig defaults EGL_SURFACE_TYPE to EGL_WINDOW_BIT, and a caller
    // that does not ask for a surface type - WebCore's is one - matches nothing
    // without it. No window surface is ever created here; the browser composites
    // through an IOSurface.
    config.surfaceType = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;

    config.minSwapInterval = 1;
    config.maxSwapInterval = 1;

    config.renderTargetFormat = GL_RGBA8;
    config.depthStencilFormat = GL_DEPTH24_STENCIL8;

    config.conformant     = EGL_OPENGL_ES2_BIT | (supportsES3 ? EGL_OPENGL_ES3_BIT_KHR : 0);
    config.renderableType = config.conformant;

    config.matchNativePixmap = EGL_NONE;

    config.colorComponentType = EGL_COLOR_COMPONENT_TYPE_FIXED_EXT;

    configs.add(config);
    return configs;
}

bool DisplayEAGL::testDeviceLost()
{
    return false;
}

egl::Error DisplayEAGL::restoreLostDevice(const egl::Display *display)
{
    UNIMPLEMENTED();
    return egl::Error(EGL_BAD_DISPLAY);
}

bool DisplayEAGL::isValidNativeWindow(EGLNativeWindowType window) const
{
    return false;
}

EAGLContext *DisplayEAGL::getEAGLContext() const
{
    return mContext;
}

void DisplayEAGL::generateExtensions(egl::DisplayExtensions *outExtensions) const
{
    outExtensions->iosurfaceClientBuffer = true;
    outExtensions->surfacelessContext    = true;

    // Contexts are virtualized so textures and semaphores can be shared globally
    outExtensions->displayTextureShareGroup   = true;
    outExtensions->displaySemaphoreShareGroup = true;

    DisplayGL::generateExtensions(outExtensions);
}

void DisplayEAGL::generateCaps(egl::Caps *outCaps) const
{
    outCaps->textureNPOT = true;
}

egl::Error DisplayEAGL::waitClient(const gl::Context *context)
{
    return egl::NoError();
}

egl::Error DisplayEAGL::waitNative(const gl::Context *context, EGLint engine)
{
    return egl::NoError();
}

egl::Error DisplayEAGL::waitUntilWorkScheduled()
{
    for (auto context : mState.contextMap)
    {
        context.second->flush();
    }
    return egl::NoError();
}

gl::Version DisplayEAGL::getMaxSupportedESVersion() const
{
    return mRenderer->getMaxSupportedESVersion();
}

egl::Error DisplayEAGL::makeCurrentSurfaceless(gl::Context *context)
{
    // mContext is always current, and EAGL is surfaceless by default.
    return egl::NoError();
}

void DisplayEAGL::initializeFrontendFeatures(angle::FrontendFeatures *features) const
{
    mRenderer->initializeFrontendFeatures(features);
}

void DisplayEAGL::populateFeatureList(angle::FeatureList *features)
{
    mRenderer->getFeatures().populateFeatureList(features);
}

RendererGL *DisplayEAGL::getRenderer() const
{
    return mRenderer.get();
}

}  // namespace rx
