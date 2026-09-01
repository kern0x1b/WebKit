/*
 * Copyright (C) 2011 Apple Inc. All rights reserved.
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

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/CALayer.h>

@class WebView;

typedef NS_ENUM(NSInteger, WebFixedPositionAnchorEdge) {
    WebFixedPositionAnchorEdgeLeft,
    WebFixedPositionAnchorEdgeRight,
    WebFixedPositionAnchorEdgeTop,
    WebFixedPositionAnchorEdgeBottom
};

// Encapsulates page content that needs to be repositioned during scrolling,
// like position:fixed layers.
// Can be called without taking the WebThread lock.
//
// It is a CALayer because UIKit puts it into its own layer tree and reads the
// constrained layers back out of it as sublayers; inheriting from NSObject
// leaves it answering none of that and the app dies on the first scroll.

@interface WebFixedPositionContent : CALayer

- (id)initWithWebView:(WebView *)webView;

- (void)scrollOrZoomChanged:(CGRect)positionedObjectsRect;
- (void)overflowScrollPositionForLayer:(CALayer *)scrollLayer changedTo:(CGPoint)scrollPosition;
- (void)didFinishScrollingOrZooming;
- (BOOL)hasFixedOrStickyPositionLayers;

@end
