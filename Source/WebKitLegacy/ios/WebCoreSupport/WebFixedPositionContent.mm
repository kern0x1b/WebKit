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

#if PLATFORM(IOS_FAMILY)

#import "WebFixedPositionContent.h"
#include <unistd.h>
#import <objc/runtime.h>
#import "WebFixedPositionContentInternal.h"

#import "WebFrameInternal.h"
#import "WebFrameViewPrivate.h"
#import "WebViewInternal.h"
#import <WebCore/WAKScrollView.h>
#import <WebCore/ChromeClient.h>
#import <WebCore/IntSize.h>
#import <WebCore/LocalFrame.h>
#import <WebCore/EventHandler.h>
#import <WebCore/ScrollingConstraints.h>
#import <WebCore/WebCoreThreadRun.h>
#import <pal/spi/cg/CoreGraphicsSPI.h>

#import <algorithm>
#import <wtf/HashMap.h>
#import <wtf/NeverDestroyed.h>
#import <wtf/RetainPtr.h>
#import <wtf/StdLibExtras.h>
#import <wtf/Threading.h>

#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <algorithm>

using namespace WebCore;

static Lock webFixedPositionContentDataLock;

struct ViewportConstrainedLayerData {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ViewportConstrainedLayerData);
    ViewportConstrainedLayerData()
        : m_enclosingAcceleratedScrollLayer(nil)
    { }
    CALayer* m_enclosingAcceleratedScrollLayer; // May be nil.
    std::unique_ptr<ViewportConstraints> m_viewportConstraints;
};

typedef HashMap<RetainPtr<CALayer>, std::unique_ptr<ViewportConstrainedLayerData>> LayerInfoMap;

struct WebFixedPositionContentData {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(WebFixedPositionContentData);
public:
    WebFixedPositionContentData(WebView *);
    ~WebFixedPositionContentData();
    
    WebView *m_webView;
    LayerInfoMap m_viewportConstrainedLayers;
};


WebFixedPositionContentData::WebFixedPositionContentData(WebView *webView)
    : m_webView(webView)
{
}

WebFixedPositionContentData::~WebFixedPositionContentData() = default;

@implementation WebFixedPositionContent {
    struct WebFixedPositionContentData* _private;
}

- (id)initWithWebView:(WebView *)webView
{
    if ((self = [super init])) {
        _private = new WebFixedPositionContentData(webView);
    }
    return self;
}

- (void)dealloc
{
    delete _private;
    [super dealloc];
}

- (void)scrollOrZoomChanged:(CGRect)positionedObjectsRect
{
    // UIKit owns the scroll view, so this is the only place the engine hears
    // where the page has been scrolled to. Without passing it on, the render
    // tree keeps the old offset: window.scrollY stays at zero, anything
    // viewport-constrained that is not on its own layer is painted where it used
    // to be, and a dialog that appears mid-scroll is not painted at all.
    WebView *webView = _private->m_webView;

    // The document shrinks to one screen whenever the page locks scrolling - a
    // modal does exactly that - and the offset UIKit hands over is still the
    // deep one from a moment ago. Feeding that through leaves the layout
    // arithmetic overflowing: window.scrollY comes back as 33554432 and nothing
    // is drawn again. Clamp it to the document that exists now.
    id documentView = [[[webView mainFrame] frameView] documentView];
    if ([documentView respondsToSelector:@selector(bounds)]) {
        CGRect documentBounds = [documentView bounds];
        CGFloat maximumOffset = std::max<CGFloat>(0, documentBounds.size.height - positionedObjectsRect.size.height);
        if (positionedObjectsRect.origin.y > maximumOffset)
            positionedObjectsRect.origin.y = maximumOffset;
        if (positionedObjectsRect.origin.y < 0)
            positionedObjectsRect.origin.y = 0;
    }

    [webView _setCustomFixedPositionLayoutRectInWebThread:positionedObjectsRect synchronize:NO];

    static CGFloat lastReportedOffset = -1;
    BOOL offsetChanged = positionedObjectsRect.origin.y != lastReportedOffset;
    lastReportedOffset = positionedObjectsRect.origin.y;

    // Told a few times a second, not sixty.
    //
    // A feed loads its next page from a scroll event, so telling the engine
    // nothing at all during a gesture means no new posts ever arrive - which is
    // what a reader sees as a page full of empty blocks. Telling it on every
    // frame is what wedged the process: each notification asks the engine to
    // re-derive its scroll state and run the page's handlers, and the next one
    // arrived before the last had returned.
    //
    // Four times a second is enough for a feed to notice and cheap enough that
    // the queue cannot grow. Note this was measured hanging at this same rate
    // before the CATransaction commit handler was fixed - back then the main
    // thread was taking the web lock on every turn of its own run loop, so
    // anything at all on top of that was fatal.
    static CFAbsoluteTime lastToldThePage;
    CFAbsoluteTime nowTelling = offsetChanged ? CFAbsoluteTimeGetCurrent() : lastToldThePage;
    if (offsetChanged && nowTelling - lastToldThePage > 0.25) {
        lastToldThePage = nowTelling;
        WebView *tellWebView = webView;
        WebThreadRun(^{
            auto* frame = [tellWebView _mainCoreFrame];
            if (!frame)
                return;
            frame->viewportOffsetChanged(LocalFrame::IncrementalScrollOffset);
            frame->eventHandler().scheduleScrollEvent();
        });
    }

    // What is deliberately not done here.
    //
    // This used to queue viewportOffsetChanged, and a scroll event with it, on
    // every frame of a scroll. Measured on the device by switching it off at
    // runtime and flicking ten times either way: with it, twenty four stalls over
    // four hundred milliseconds and the application ending up hung - the web
    // thread parked inside a JavaScript event listener holding the web lock, taps
    // ignored, the frame counter stopped. Without it, one stall, and the scroll
    // view running freely to an offset of 2946.
    //
    // The engine still learns where the window is: the rectangle above is
    // published on every frame, and the pinned layers are moved below. What is
    // gone is the per-frame demand that the engine re-derive its scroll state and
    // run the page's handlers, which this device cannot pay for sixty times a
    // second. That work happens once, when the scroll comes to rest, from
    // didFinishScrollingOrZooming.

    Locker locker { webFixedPositionContentDataLock };

#if defined(WEBKIT_IOS6)
    {
        static int recordFixed = -1;
        if (recordFixed < 0)
            recordFixed = access("/tmp/native-weblock-on", F_OK) == 0 ? 1 : 0;
        if (recordFixed) {
            static CFAbsoluteTime lastReport;
            CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
            if (now - lastReport > 1.0) {
                lastReport = now;
                FILE *log = fopen("/tmp/native-fixed.log", "a");
                if (log) {
                    fprintf(log, "%.3f offset %.0f, %u constrained layers\n", now,
                        (double)positionedObjectsRect.origin.y,
                        (unsigned)_private->m_viewportConstrainedLayers.size());
                    fclose(log);
                }
            }
        }
    }
#endif

    const LayerInfoMap& constrainedLayers = _private->m_viewportConstrainedLayers;
    if (constrainedLayers.isEmpty())
        return;

    [CATransaction begin];
    [CATransaction setDisableActions:YES];

    LayerInfoMap::const_iterator end = constrainedLayers.end();
    for (LayerInfoMap::const_iterator it = constrainedLayers.begin(); it != end; ++it) {
        CALayer *layer = it->key.get();
        ViewportConstrainedLayerData* constraintData = it->value.get();
        const ViewportConstraints& constraints = *(constraintData->m_viewportConstraints.get());

        switch (constraints.constraintType()) {
        case ViewportConstraints::FixedPositionConstraint: {
            auto& fixedConstraints = downcast<FixedPositionViewportConstraints>(constraints);

            auto layerPosition = fixedConstraints.viewportRelativeLayerPosition(positionedObjectsRect);

            CGRect layerBounds = [layer bounds];
            CGPoint anchorPoint = [layer anchorPoint];

            // viewportRelativeLayerPosition is in the coordinates of the root
            // content layer, and a pinned layer is not always a direct child of
            // it: its parent is a structural layer that the engine moves as it
            // lays the fixed element out, so that offset has to come back out or
            // it is applied twice and the bar slides off the screen.
            //
            // The child's position therefore *should* change every frame - it is
            // what cancels the parent's movement. Holding it constant was tried
            // and is wrong: measured on the device by the only figure that
            // matters, the bar's position on screen (parent + child - scroll),
            // the cancelling form holds 37 across 186 consecutive samples with a
            // spread of zero, and the constant form wanders between -25 and 37.
            CGPoint parentOffset = CGPointZero;
            if (CALayer *superlayer = [layer superlayer])
                parentOffset = [superlayer frame].origin;

            CGPoint newPosition = CGPointMake(layerPosition.x() - parentOffset.x - constraints.alignmentOffset().width() + anchorPoint.x * layerBounds.size.width,
                layerPosition.y() - parentOffset.y - constraints.alignmentOffset().height() + anchorPoint.y * layerBounds.size.height);

            // A learned constant was tried here and removed.
            //
            // The idea was to remember where a pinned bar belongs on screen the
            // first time it is computed from a fresh constraint, and afterwards
            // derive its position from that constant and the current scroll,
            // instead of from constraints that may have aged. It fixed bars that
            // left the screen entirely, and then produced a worse fault: the
            // constant is learned once, and if that one computation lands while
            // the site is mid-way through hiding its own header, the bar is
            // pinned to the middle of the screen for the rest of the session.
            // Seen in a screenshot from the device with the header floating over
            // the feed.
            //
            // What makes the plain arithmetic correct is the constraint being
            // paired with the layout that produced it, which is done in
            // LocalFrameViewLayoutContext.

            [layer setPosition:newPosition];

#if defined(WEBKIT_IOS6)
            {
                static int recordBars = -1;
                if (recordBars < 0)
                    recordBars = access("/tmp/native-bars-on", F_OK) == 0 ? 1 : 0;
                if (recordBars) {
                    static FILE *barLog;
                    if (!barLog) {
                        barLog = fopen("/tmp/native-bars.log", "w");
                        if (barLog)
                            setvbuf(barLog, NULL, _IOLBF, 0);
                    }
                    static CFAbsoluteTime lastChainReport;
                    CFAbsoluteTime chainNow = CFAbsoluteTimeGetCurrent();
                    if (barLog && chainNow - lastChainReport > 3.0) {
                        lastChainReport = chainNow;
                        CALayer *walk = layer;
                        fprintf(barLog, "chain for %p:", layer);
                        for (int level = 0; level < 5 && walk; level++) {
                            CGRect f = [walk frame];
                            fprintf(barLog, " [%s %p @%.0f,%.0f %.0fx%.0f]", object_getClassName(walk), walk,
                                (double)f.origin.x, (double)f.origin.y, (double)f.size.width, (double)f.size.height);
                            walk = [walk superlayer];
                        }
                        fprintf(barLog, "\n");
                    }
                    if (barLog) {
                        CGFloat absoluteY = newPosition.y - anchorPoint.y * layerBounds.size.height;
                        for (CALayer *walk = [layer superlayer]; walk; walk = [walk superlayer]) {
                            CGRect walkFrame = [walk frame];
                            absoluteY += walkFrame.origin.y;
                            CGPoint walkBoundsOrigin = [walk bounds].origin;
                            absoluteY -= walkBoundsOrigin.y;
                        }
                        fprintf(barLog, "%.3f absolute y %.1f\n", CFAbsoluteTimeGetCurrent(), (double)absoluteY);
                        fprintf(barLog, "%.3f layer %p h %.0f scroll %.1f want %.1f parent %.1f align %.1f -> pos %.1f\n",
                            CFAbsoluteTimeGetCurrent(), layer, (double)layerBounds.size.height,
                            (double)positionedObjectsRect.origin.y, (double)layerPosition.y(),
                            (double)parentOffset.y, (double)constraints.alignmentOffset().height(),
                            (double)newPosition.y);
                    }
                }
            }
#endif
            break;
        }
        case ViewportConstraints::StickyPositionConstraint: {
            auto& stickyConstraints = downcast<StickyPositionViewportConstraints>(constraints);

            FloatPoint layerPosition = stickyConstraints.anchorLayerPositionForConstrainingRect(positionedObjectsRect);

            CGRect layerBounds = [layer bounds];
            CGPoint anchorPoint = [layer anchorPoint];

            // viewportRelativeLayerPosition is in the coordinates of the layer
            // this port composites everything against - the root content layer.
            // A viewport-constrained layer is not always a direct child of it:
            // when it is nested under a structural layer that already carries
            // the element's document position, setting that position again adds
            // the offset twice and the bar slides off the screen.
            CGPoint parentOffset = CGPointZero;
            if (CALayer *superlayer = [layer superlayer]) {
                CGRect superFrame = [superlayer frame];
                parentOffset = superFrame.origin;
            }

            CGPoint newPosition = CGPointMake(layerPosition.x() - parentOffset.x - constraints.alignmentOffset().width() + anchorPoint.x * layerBounds.size.width,
                layerPosition.y() - parentOffset.y - constraints.alignmentOffset().height() + anchorPoint.y * layerBounds.size.height);
            [layer setPosition:newPosition];
            break;
        }
        }
    }

    [CATransaction commit];
}

- (void)overflowScrollPositionForLayer:(CALayer *)scrollLayer changedTo:(CGPoint)scrollPosition
{
    Locker locker { webFixedPositionContentDataLock };

    LayerInfoMap::const_iterator end = _private->m_viewportConstrainedLayers.end();
    for (LayerInfoMap::const_iterator it = _private->m_viewportConstrainedLayers.begin(); it != end; ++it) {
        CALayer *layer = it->key.get();
        ViewportConstrainedLayerData* constraintData = it->value.get();
        
        if (constraintData->m_enclosingAcceleratedScrollLayer == scrollLayer) {
            const StickyPositionViewportConstraints& stickyConstraints = static_cast<const StickyPositionViewportConstraints&>(*(constraintData->m_viewportConstraints.get()));
            FloatRect constrainingRectAtLastLayout = stickyConstraints.constrainingRectAtLastLayout();
            FloatRect scrolledConstrainingRect = FloatRect(scrollPosition.x, scrollPosition.y, constrainingRectAtLastLayout.width(), constrainingRectAtLastLayout.height());
            FloatPoint layerPosition = stickyConstraints.anchorLayerPositionForConstrainingRect(scrolledConstrainingRect);

            CGRect layerBounds = [layer bounds];
            CGPoint anchorPoint = [layer anchorPoint];
            CGPoint parentOffset = CGPointZero;
            if (CALayer *superlayer = [layer superlayer])
                parentOffset = [superlayer frame].origin;
            CGPoint newPosition = CGPointMake(layerPosition.x() - parentOffset.x - stickyConstraints.alignmentOffset().width() + anchorPoint.x * layerBounds.size.width,
                                              layerPosition.y() - parentOffset.y - stickyConstraints.alignmentOffset().height() + anchorPoint.y * layerBounds.size.height);
            [layer setPosition:newPosition];
        }
    }

}

// FIXME: share code with 'sendScrollEvent'?
- (void)didFinishScrollingOrZooming
{

    WebView *finishedWebView = _private->m_webView;
    WebThreadRun(^{
        if (auto* frame = [finishedWebView _mainCoreFrame])
            frame->viewportOffsetChanged(LocalFrame::CompletedScrollOffset);
        if (auto* frame = [finishedWebView _mainCoreFrame])
            frame->eventHandler().scheduleScrollEvent();
    });
}

- (void)setViewportConstrainedLayers:(WTF::HashMap<CALayer *, std::unique_ptr<WebCore::ViewportConstraints>>&)layerMap stickyContainerMap:(const WTF::HashMap<CALayer*, CALayer*>&)stickyContainers
{
    Locker locker { webFixedPositionContentDataLock };

    _private->m_viewportConstrainedLayers.clear();

    for (auto& layerAndConstraints : layerMap) {
        CALayer* layer = layerAndConstraints.key;
        auto layerData = makeUnique<ViewportConstrainedLayerData>();

        layerData->m_enclosingAcceleratedScrollLayer = stickyContainers.get(layer);
        layerData->m_viewportConstraints = WTF::move(layerAndConstraints.value);

        _private->m_viewportConstrainedLayers.set(layer, WTF::move(layerData));
    }

}

- (BOOL)hasFixedOrStickyPositionLayers
{
    Locker locker { webFixedPositionContentDataLock };
    return !_private->m_viewportConstrainedLayers.isEmpty();
}

@end

#endif // PLATFORM(IOS_FAMILY)
