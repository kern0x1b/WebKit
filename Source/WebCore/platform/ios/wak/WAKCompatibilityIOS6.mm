/*
 * Copyright (C) 2005-2021 Apple Inc. All rights reserved.
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
#import <objc/runtime.h>


#if PLATFORM(IOS_FAMILY)

#import "LegacyTileCache.h"
#import "WAKScrollView.h"
#import "WAKViewInternal.h"
#import "WAKWindow.h"
#import "WKWindow.h"
#import "WebEvent.h"

@interface WAKWindow (WAKCompatibilityIOS6)
@end

@implementation WAKWindow (WAKCompatibilityIOS6)

+ (WAKWindow *)_wrapperForWindowRef:(WKWindowRef)window
{
    return window ? window->wakWindow : nil;
}

- (BOOL)hasPendingDraw
{
    return NO;
}

- (BOOL)makeViewFirstResponder:(WAKView *)view
{
    return [self makeFirstResponder:view];
}

- (void)setAcceleratedDrawingEnabled:(BOOL)enabled
{
    UNUSED_PARAM(enabled);
}

- (void)setTileBordersVisible:(BOOL)visible
{
    if (LegacyTileCache* cache = [self tileCache])
        cache->setTileBordersVisible(visible);
}

- (void)setTilePaintCountsVisible:(BOOL)visible
{
    if (LegacyTileCache* cache = [self tileCache])
        cache->setTilePaintCountersVisible(visible);
}

@end

@interface WAKView (WAKCompatibilityIOS6)
@end

@implementation WAKView (WAKCompatibilityIOS6)

- (WAKView *)_frame
{
    return self;
}

- (id)_webView
{
    return [self _web_superviewOfClass:NSClassFromString(@IOS6_CLASS_NAME(WebView))];
}

- (id)_web_superviewOfClass:(Class)viewClass
{
    for (WAKView *view = [self superview]; view; view = [view superview]) {
        if ([view isKindOfClass:viewClass])
            return view;
    }
    return nil;
}

- (id)_web_parentWebFrameView
{
    return [self _web_superviewOfClass:NSClassFromString(@IOS6_CLASS_NAME(WebFrameView))];
}

- (BOOL)_web_firstResponderIsSelfOrDescendantView
{
    WAKView *responder = [[self window] firstResponder];
    for (WAKView *view = responder; view; view = [view superview]) {
        if (view == self)
            return YES;
    }
    return NO;
}

- (CGRect)_web_convertRect:(CGRect)rect toView:(WAKView *)view
{
    return [self convertRect:rect toView:view];
}

- (void)_web_addDescendantWebHTMLViewsToArray:(NSMutableArray *)array
{
    Class htmlView = NSClassFromString(@IOS6_CLASS_NAME(WebHTMLView));
    for (WAKView *subview in [self subviews]) {
        if (htmlView && [subview isKindOfClass:htmlView])
            [array addObject:subview];
        [subview _web_addDescendantWebHTMLViewsToArray:array];
    }
}

@end

@interface WAKScrollView (WAKCompatibilityIOS6)
@end

@implementation WAKScrollView (WAKCompatibilityIOS6)

- (CGRect)actualDocumentVisibleRect
{
    return [self documentVisibleRect];
}

@end

@interface WebEvent (WAKCompatibilityIOS6)
@end

@implementation WebEvent (WAKCompatibilityIOS6)

static const void* webEventCharacterSetKey = &webEventCharacterSetKey;
static const void* webEventPopupVariantKey = &webEventPopupVariantKey;

- (int)characterSet
{
    NSNumber *stored = objc_getAssociatedObject(self, webEventCharacterSetKey);
    return stored ? [stored intValue] : WebEventCharacterSetUnicode;
}

- (NSString *)_characterSetDescription
{
    switch ([self characterSet]) {
    case WebEventCharacterSetASCII:
        return @"ASCII";
    case WebEventCharacterSetSymbol:
        return @"Symbol";
    case WebEventCharacterSetDingbats:
        return @"Dingbats";
    default:
        return @"Unicode";
    }
}

- (BOOL)isPopupVariant
{
    return [objc_getAssociatedObject(self, webEventPopupVariantKey) boolValue];
}

- (WebEvent *)initWithKeyEventType:(WebEventType)type timeStamp:(CFTimeInterval)timeStamp characters:(NSString *)characters charactersIgnoringModifiers:(NSString *)charactersIgnoringModifiers modifiers:(WebEventFlags)modifiers isRepeating:(BOOL)isRepeating isPopupVariant:(BOOL)isPopupVariant keyCode:(uint16_t)keyCode isTabKey:(BOOL)isTabKey characterSet:(int)characterSet
{
    self = [self initWithKeyEventType:type timeStamp:timeStamp characters:characters charactersIgnoringModifiers:charactersIgnoringModifiers modifiers:modifiers isRepeating:isRepeating withFlags:0 withInputManagerHint:nil keyCode:keyCode isTabKey:isTabKey];
    if (self) {
        objc_setAssociatedObject(self, webEventCharacterSetKey, [NSNumber numberWithInt:characterSet], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        objc_setAssociatedObject(self, webEventPopupVariantKey, [NSNumber numberWithBool:isPopupVariant], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return self;
}

@end

#endif // PLATFORM(IOS_FAMILY)
