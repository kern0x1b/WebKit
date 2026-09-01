/*
 * Copyright (C) 2008-2017 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#import "config.h"
#import "ThemeIOS.h"

#if PLATFORM(IOS_FAMILY)

#import <pal/ios/UIKitSoftLink.h>
#import <wtf/NeverDestroyed.h>

namespace WebCore {

Theme& Theme::singleton()
{
    static NeverDestroyed<ThemeIOS> theme;
    return theme;
}

// UIAccessibilityDarkerSystemColorsEnabled and UIAccessibilityIsReduceMotionEnabled
// are iOS 8, UIAccessibilityIsOnOffSwitchLabelsEnabled is iOS 7. Soft linking
// them here is fatal rather than merely unavailable, and a system without the
// setting has it turned off.
InterfaceContrastPreference ThemeIOS::userPreferredContrast() const
{
#if defined(WEBKIT_IOS6)
    return InterfaceContrastPreference::NoPreference;
#else
    if (PAL::softLink_UIKit_UIAccessibilityDarkerSystemColorsEnabled())
        return InterfaceContrastPreference::MoreContrast;
    return InterfaceContrastPreference::NoPreference;
#endif
}

bool ThemeIOS::userPrefersReducedMotion() const
{
#if defined(WEBKIT_IOS6)
    return false;
#else
    return PAL::softLink_UIKit_UIAccessibilityIsReduceMotionEnabled();
#endif
}

bool ThemeIOS::userPrefersOnOffLabels() const
{
#if defined(WEBKIT_IOS6)
    return false;
#else
    return PAL::softLink_UIKit_UIAccessibilityIsOnOffSwitchLabelsEnabled();
#endif
}

}

#endif // PLATFORM(IOS_FAMILY)
