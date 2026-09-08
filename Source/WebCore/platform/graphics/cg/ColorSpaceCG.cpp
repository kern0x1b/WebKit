/*
 * Copyright (C) 2020-2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ColorSpaceCG.h"

#if USE(CG)

#include <mutex>
#include <pal/spi/cg/CoreGraphicsSPI.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RetainPtr.h>

namespace WebCore {

template<const CFStringRef& colorSpaceNameGlobalConstant> static CGColorSpaceRef namedColorSpace()
{
    static LazyNeverDestroyed<RetainPtr<CGColorSpaceRef>> colorSpace;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
#if defined(WEBKIT_IOS6)
        // This CoreGraphics has one RGB colour space and does not answer to any
        // of these names — CGColorSpaceCreateWithName(kCGColorSpaceSRGB) included,
        // which returns nothing here. Its device RGB is sRGB: the screen is an
        // sRGB panel and there is no colour management to select between spaces.
        colorSpace.construct(adoptCF(CGColorSpaceCreateDeviceRGB()));
#else
        colorSpace.construct(adoptCF(CGColorSpaceCreateWithName(RetainPtr { colorSpaceNameGlobalConstant }.get())));
#endif
        ASSERT(colorSpace.get());
    });
    return colorSpace.get().get();
}

template<const CFStringRef& colorSpaceNameGlobalConstant> static CGColorSpaceRef extendedNamedColorSpace()
{
    static LazyNeverDestroyed<RetainPtr<CGColorSpaceRef>> colorSpace;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
#if defined(WEBKIT_IOS6)
        colorSpace.construct(RetainPtr { namedColorSpace<colorSpaceNameGlobalConstant>() });
#else
        colorSpace.construct(adoptCF(CGColorSpaceCreateExtended(RetainPtr { namedColorSpace<colorSpaceNameGlobalConstant>() }.get())));
#endif
        ASSERT(colorSpace.get());
    });
    return colorSpace.get().get();
}

CGColorSpaceRef sRGBColorSpaceSingleton()
{
    return namedColorSpace<kCGColorSpaceSRGB>();
}

CGColorSpaceRef adobeRGB1998ColorSpaceSingleton()
{
    return namedColorSpace<kCGColorSpaceAdobeRGB1998>();
}

CGColorSpaceRef displayP3ColorSpaceSingleton()
{
    return namedColorSpace<kCGColorSpaceDisplayP3>();
}

CGColorSpaceRef extendedAdobeRGB1998ColorSpaceSingleton()
{
    return extendedNamedColorSpace<kCGColorSpaceAdobeRGB1998>();
}

CGColorSpaceRef extendedDisplayP3ColorSpaceSingleton()
{
    return namedColorSpace<kCGColorSpaceExtendedDisplayP3>();
}

CGColorSpaceRef extendedITUR_2020ColorSpaceSingleton()
{
    return namedColorSpace<kCGColorSpaceExtendedITUR_2020>();
}

CGColorSpaceRef extendedLinearDisplayP3ColorSpaceSingleton()
{
    return namedColorSpace<kCGColorSpaceExtendedLinearDisplayP3>();
}

CGColorSpaceRef extendedLinearSRGBColorSpaceSingleton()
{
    return namedColorSpace<kCGColorSpaceExtendedLinearSRGB>();
}

CGColorSpaceRef extendedROMMRGBColorSpaceSingleton()
{
    return extendedNamedColorSpace<kCGColorSpaceROMMRGB>();
}

CGColorSpaceRef extendedSRGBColorSpaceSingleton()
{
    return namedColorSpace<kCGColorSpaceExtendedSRGB>();
}

CGColorSpaceRef ITUR_2020ColorSpaceSingleton()
{
    return namedColorSpace<kCGColorSpaceITUR_2020>();
}

CGColorSpaceRef linearDisplayP3ColorSpaceSingleton()
{
    return namedColorSpace<kCGColorSpaceLinearDisplayP3>();
}

CGColorSpaceRef linearSRGBColorSpaceSingleton()
{
#if defined(WEBKIT_IOS6)
    // Every named space falls back to device RGB on this CoreGraphics, which is
    // gamma encoded. Handing that back here would be wrong rather than merely
    // approximate: linearRGB is the colour space SVG filters interpolate in by
    // default, so every filter would be computed on gamma encoded pixels.
    // A calibrated space with a gamma of one and the sRGB primaries is genuinely
    // linear and can be built on this release.
    static LazyNeverDestroyed<RetainPtr<CGColorSpaceRef>> colorSpace;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        const CGFloat whitePoint[3] = { 0.9505, 1.0, 1.089 }; // D65
        const CGFloat blackPoint[3] = { 0, 0, 0 };
        const CGFloat gamma[3] = { 1, 1, 1 };
        const CGFloat matrix[9] = {
            0.4124, 0.2126, 0.0193,
            0.3576, 0.7152, 0.1192,
            0.1805, 0.0722, 0.9505
        };
        colorSpace.construct(adoptCF(CGColorSpaceCreateCalibratedRGB(whitePoint, blackPoint, gamma, matrix)));
        if (!colorSpace.get())
            colorSpace.construct(adoptCF(CGColorSpaceCreateDeviceRGB()));
    });
    return colorSpace.get().get();
#else
    return namedColorSpace<kCGColorSpaceLinearSRGB>();
#endif
}

CGColorSpaceRef ROMMRGBColorSpaceSingleton()
{
    return namedColorSpace<kCGColorSpaceROMMRGB>();
}

CGColorSpaceRef xyzD50ColorSpaceSingleton()
{
    return namedColorSpace<kCGColorSpaceGenericXYZ>();
}

// FIXME: Figure out how to create a CoreGraphics XYZ-D65 color space and add a xyzD65ColorSpaceRef(). Perhaps CGColorSpaceCreateCalibratedRGB() with identify black point, D65 white point, and identity matrix.

std::optional<ColorSpace> colorSpaceForCGColorSpace(CGColorSpaceRef colorSpace)
{
    // First test for the four most common spaces, sRGB, Extended sRGB, DisplayP3 and Linear sRGB, and then test
    // the reset in alphabetical order.
    // FIXME: Consider using a HashMap (with CFHash based keys) rather than the linear set of tests.

    if (CGColorSpaceEqualToColorSpace(colorSpace, sRGBColorSpaceSingleton()))
        return ColorSpace::SRGB;

    if (CGColorSpaceEqualToColorSpace(colorSpace, extendedSRGBColorSpaceSingleton()))
        return ColorSpace::ExtendedSRGB;

    if (CGColorSpaceEqualToColorSpace(colorSpace, displayP3ColorSpaceSingleton()))
        return ColorSpace::DisplayP3;

    if (CGColorSpaceEqualToColorSpace(colorSpace, linearSRGBColorSpaceSingleton()))
        return ColorSpace::LinearSRGB;

    if (CGColorSpaceEqualToColorSpace(colorSpace, adobeRGB1998ColorSpaceSingleton()))
        return ColorSpace::A98RGB;

    if (CGColorSpaceEqualToColorSpace(colorSpace, extendedAdobeRGB1998ColorSpaceSingleton()))
        return ColorSpace::ExtendedA98RGB;

    if (CGColorSpaceEqualToColorSpace(colorSpace, extendedDisplayP3ColorSpaceSingleton()))
        return ColorSpace::ExtendedDisplayP3;

    if (CGColorSpaceEqualToColorSpace(colorSpace, extendedLinearDisplayP3ColorSpaceSingleton()))
        return ColorSpace::ExtendedLinearDisplayP3;

    if (CGColorSpaceEqualToColorSpace(colorSpace, extendedLinearSRGBColorSpaceSingleton()))
        return ColorSpace::ExtendedLinearSRGB;

    if (CGColorSpaceEqualToColorSpace(colorSpace, extendedITUR_2020ColorSpaceSingleton()))
        return ColorSpace::ExtendedRec2020;

    if (CGColorSpaceEqualToColorSpace(colorSpace, extendedROMMRGBColorSpaceSingleton()))
        return ColorSpace::ExtendedProPhotoRGB;

    if (CGColorSpaceEqualToColorSpace(colorSpace, ITUR_2020ColorSpaceSingleton()))
        return ColorSpace::Rec2020;

    if (CGColorSpaceEqualToColorSpace(colorSpace, linearDisplayP3ColorSpaceSingleton()))
        return ColorSpace::LinearDisplayP3;

    if (CGColorSpaceEqualToColorSpace(colorSpace, ROMMRGBColorSpaceSingleton()))
        return ColorSpace::ProPhotoRGB;

    if (CGColorSpaceEqualToColorSpace(colorSpace, xyzD50ColorSpaceSingleton()))
        return ColorSpace::XYZ_D50;

    // FIXME: Add support for remaining color spaces to support more direct conversions.

    return std::nullopt;
}

}

#endif // USE(CG)
