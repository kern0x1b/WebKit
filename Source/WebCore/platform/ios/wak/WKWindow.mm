/*
 * Copyright (C) 2005, 2006, 2007, 2008, 2009 Apple Inc. All rights reserved.
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


#import "config.h"
#import "WKWindow.h"

#if PLATFORM(IOS_FAMILY)

#import "WAKViewInternal.h"
#import "WAKWindow.h"
#import "WKUtilities.h"

static void WKWindowDealloc(WAKObjectRef object);

WKClassInfo WKWindowClassInfo = { &WAKObjectClass, "WKWindow", WKWindowDealloc };

static void WKWindowDealloc(WAKObjectRef object)
{
    WKWindowRef window = reinterpret_cast<WKWindowRef>(object);
    window->wakWindow = nil;
}

WKWindowRef WKWindowCreate(WAKWindow *wakWindow, CGRect contentRect)
{
    if (!wakWindow) {
        WTFLogAlways("WKWindowCreate: invalid parameter");
        return 0;
    }

    WKWindowRef window = reinterpret_cast<WKWindowRef>(const_cast<void*>(WKCreateObjectWithSize(sizeof(struct WKWindow), &WKWindowClassInfo)));
    if (!window)
        return 0;

    window->wakWindow = wakWindow;
    [wakWindow setContentRect:contentRect];

    return window;
}

void WKWindowClose(WKWindowRef window)
{
    if (!window) {
        WTFLogAlways("WKWindowClose: invalid parameter");
        return;
    }

    [window->wakWindow close];
}

void WKWindowSetContentView(WKWindowRef window, WKViewRef aView)
{
    if (!window) {
        WTFLogAlways("WKWindowSetContentView: invalid parameter");
        return;
    }

    [window->wakWindow setContentView:WAKViewForWKViewRef(aView)];
}

WKViewRef WKWindowGetContentView(WKWindowRef window)
{
    if (!window) {
        WTFLogAlways("WKWindowGetContentView: invalid parameter");
        return 0;
    }

    return [[window->wakWindow contentView] _viewRef];
}

bool WKWindowMakeFirstResponder(WKWindowRef window, WKViewRef view)
{
    if (!window) {
        WTFLogAlways("WKWindowMakeFirstResponder: invalid parameter");
        return false;
    }

    return [window->wakWindow makeFirstResponder:WAKViewForWKViewRef(view)];
}

WKViewRef WKWindowFirstResponder(WKWindowRef window)
{
    if (!window) {
        WTFLogAlways("WKWindowFirstResponder: invalid parameter");
        return 0;
    }

    return [[window->wakWindow firstResponder] _viewRef];
}

WKViewRef WKWindowNewFirstResponderAfterResigning(WKWindowRef window)
{
    if (!window) {
        WTFLogAlways("WKWindowNewFirstResponderAfterResigning: invalid parameter");
        return 0;
    }

    return [[window->wakWindow _newFirstResponderAfterResigning] _viewRef];
}

void WKWindowPrepareForDrawing(WKWindowRef window)
{
    if (!window) {
        WTFLogAlways("WKWindowPrepareForDrawing: invalid parameter");
        return;
    }

    [window->wakWindow layoutTiles];
}

#endif // PLATFORM(IOS_FAMILY)
