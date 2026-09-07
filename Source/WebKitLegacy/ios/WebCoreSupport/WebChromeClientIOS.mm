/*
 * Copyright (C) 2008-2025 Apple Inc. All rights reserved.
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

#import "WebChromeClientIOS.h"
#import <WebCore/UserGestureIndicator.h>
#import <WebCore/HTMLSelectElement.h>
#import <WebCore/HTMLTextFormControlElement.h>

#if PLATFORM(IOS_FAMILY)

#import "DOMNodeInternal.h"
#import "PopupMenuIOS.h"
#import "SearchPopupMenuIOS.h"
#import "WebDelegateImplementationCaching.h"
#import "WebFixedPositionContent.h"
#import "WebFixedPositionContentInternal.h"
#import "WebFormDelegate.h"
#import "WebFrameIOS.h"
#import "WebFrameInternal.h"
#import "WebHistoryItemInternal.h"
#import "WebOpenPanelResultListener.h"
#import "WebUIDelegate.h"
#import "WebUIDelegatePrivate.h"
#import "WebUIKitDelegate.h"
#import "WebView.h"
#import "WebViewInternal.h"
#import "WebViewPrivate.h"
#import <WebCore/ContentChangeObserver.h>
#import <WebCore/DisabledAdaptations.h>
#import <WebCore/FileChooser.h>
#import <WebCore/FloatRect.h>
#import <WebCore/FocusControllerTypes.h>
#import <WebCore/FocusOptions.h>
#import <WebCore/Frame.h>
#import <WebCore/FrameDestructionObserverInlines.h>
#import <WebCore/GraphicsLayer.h>
#import <WebCore/HTMLInputElement.h>
#import <WebCore/HTMLNames.h>
#import <WebCore/Icon.h>
#import <WebCore/IntRect.h>
#import <WebCore/LocalFrameInlines.h>
#import <WebCore/LocalFrameView.h>
#import <WebCore/NodeDocument.h>
#import <WebCore/PlatformScreen.h>
#import <WebCore/RenderBox.h>
#import <WebCore/RenderObject.h>
#import <WebCore/ScrollingConstraints.h>
#import <WebCore/WAKWindow.h>
#import <WebCore/WebCoreThreadMessage.h>
#import <wtf/HashMap.h>
#import <wtf/RefPtr.h>
#import <wtf/RuntimeApplicationChecks.h>
#import <wtf/TZoneMallocInlines.h>
#import <wtf/cocoa/VectorCocoa.h>

NSString * const WebOpenPanelConfigurationAllowMultipleFilesKey = @"WebOpenPanelConfigurationAllowMultipleFilesKey";
NSString * const WebOpenPanelConfigurationMediaCaptureTypeKey = @"WebOpenPanelConfigurationMediaCaptureTypeKey";
NSString * const WebOpenPanelConfigurationMimeTypesKey = @"WebOpenPanelConfigurationMimeTypesKey";

using namespace WebCore;

#if ENABLE(MEDIA_CAPTURE)

static WebMediaCaptureType webMediaCaptureType(MediaCaptureType type)
{
    switch (type) {
    case MediaCaptureType::MediaCaptureTypeNone:
        return WebMediaCaptureTypeNone;
    case MediaCaptureType::MediaCaptureTypeUser:
        return WebMediaCaptureTypeUser;
    case MediaCaptureType::MediaCaptureTypeEnvironment:
        return WebMediaCaptureTypeEnvironment;
    }

    ASSERT_NOT_REACHED();
    return WebMediaCaptureTypeNone;
}

#endif

WTF_MAKE_TZONE_ALLOCATED_IMPL(WebChromeClientIOS);

#if defined(WEBKIT_IOS6)
@interface NSObject (RevViewportForFrameCompat)
- (void)webView:(WebView *)webView didReceiveViewportArguments:(NSDictionary *)arguments forFrame:(WebFrame *)frame;
@end

static inline bool uiKitDelegateImplements(WebView *webView, SEL selector)
{
    return [[webView _UIKitDelegate] respondsToSelector:selector];
}

static void callUIKitDelegateAsync(WebView *webView, SEL selector)
{
    if (![webView _UIKitDelegateForwarder])
        return;

    RetainPtr<id> delegate = [webView _UIKitDelegate];
    if (![delegate respondsToSelector:selector])
        return;

    if (!WebThreadIsCurrent()) {
        [delegate.get() performSelector:selector withObject:webView];
        return;
    }

    RunLoop::mainSingleton().dispatch([delegate = WTF::move(delegate), webView = retainPtr(webView), selector] {
        [delegate.get() performSelector:selector withObject:webView.get()];
    });
}
#endif

void WebChromeClientIOS::setWindowRect(const WebCore::FloatRect& r)
{
    [[webView() _UIDelegateForwarder] webView:webView() setFrame:r];
}

FloatRect WebChromeClientIOS::windowRect() const
{
    CGRect windowRect = [[webView() _UIDelegateForwarder] webViewFrame:webView()];
    return enclosingIntRect(windowRect);
}

void WebChromeClientIOS::focus()
{
    [[webView() _UIDelegateForwarder] webViewFocus:webView()];
}

void WebChromeClientIOS::runJavaScriptAlert(LocalFrame& frame, const WTF::String& message)
{
    WebThreadLockPushModal();
    [[webView() _UIDelegateForwarder] webView:webView() runJavaScriptAlertPanelWithMessage:message.createNSString().get() initiatedByFrame:kit(&frame)];
    WebThreadLockPopModal();
}

bool WebChromeClientIOS::runJavaScriptConfirm(LocalFrame& frame, const WTF::String& message)
{
    WebThreadLockPushModal();
    bool result = [[webView() _UIDelegateForwarder] webView:webView() runJavaScriptConfirmPanelWithMessage:message.createNSString().get() initiatedByFrame:kit(&frame)];
    WebThreadLockPopModal();
    return result;
}

bool WebChromeClientIOS::runJavaScriptPrompt(LocalFrame& frame, const WTF::String& prompt, const WTF::String& defaultText, WTF::String& result)
{
    WebThreadLockPushModal();
    result = [[webView() _UIDelegateForwarder] webView:webView() runJavaScriptTextInputPanelWithPrompt:prompt.createNSString().get() defaultText:defaultText.createNSString().get() initiatedByFrame:kit(&frame)];
    WebThreadLockPopModal();
    return !result.isNull();
}

void WebChromeClientIOS::runOpenPanel(LocalFrame&, FileChooser& chooser)
{
    auto& settings = chooser.settings();
    BOOL allowMultipleFiles = settings.allowsMultipleFiles;
    auto listener = adoptNS([[WebOpenPanelResultListener alloc] initWithChooser:chooser]);

    WebMediaCaptureType captureType = WebMediaCaptureTypeNone;
#if ENABLE(MEDIA_CAPTURE)
    captureType = webMediaCaptureType(settings.mediaCaptureType);
#endif
    NSDictionary *configuration = @{
        WebOpenPanelConfigurationAllowMultipleFilesKey: @(allowMultipleFiles),
        WebOpenPanelConfigurationMimeTypesKey: createNSArray(settings.acceptMIMETypes).get(),
        WebOpenPanelConfigurationMediaCaptureTypeKey: @(captureType)
    };

    if (WebThreadIsCurrent()) {
        RunLoop::mainSingleton().dispatch([this, listener = WTF::move(listener), configuration = retainPtr(configuration)] {
            [[webView() _UIKitDelegateForwarder] webView:webView() runOpenPanelForFileButtonWithResultListener:listener.get() configuration:configuration.get()];
        });
    } else
        [[webView() _UIKitDelegateForwarder] webView:webView() runOpenPanelForFileButtonWithResultListener:listener.get() configuration:configuration];
}

void WebChromeClientIOS::showShareSheet(ShareDataWithParsedURL&&, CompletionHandler<void(bool)>&&)
{
}

#if ENABLE(IOS_TOUCH_EVENTS) || ENABLE(TOUCH_EVENTS)

void WebChromeClientIOS::didPreventDefaultForEvent()
{
#if defined(WEBKIT_IOS6)
    if (!uiKitDelegateImplements(webView(), @selector(webViewDidPreventDefaultForEvent:)))
        return;
#endif
    [[webView() _UIKitDelegateForwarder] webViewDidPreventDefaultForEvent:webView()];
}

#endif

void WebChromeClientIOS::didReceiveMobileDocType(bool isMobileDoctype)
{
    if (isMobileDoctype)
        [[webView() _UIKitDelegateForwarder] webViewDidReceiveMobileDocType:webView()];
}

void WebChromeClientIOS::setNeedsScrollNotifications(WebCore::LocalFrame& frame, bool flag)
{
    [[webView() _UIKitDelegateForwarder] webView:webView() needsScrollNotifications:[NSNumber numberWithBool:flag] forFrame:kit(&frame)];
}

static inline NSString *nameForViewportFitValue(ViewportFit value)
{
    switch (value) {
    case ViewportFit::Auto:
        return WebViewportFitAutoValue;
    case ViewportFit::Contain:
        return WebViewportFitContainValue;
    case ViewportFit::Cover:
        return WebViewportFitCoverValue;
    }
    return WebViewportFitAutoValue;
}

// UIKit lays the page out from these numbers, and the sentinel this WebKit uses
// for device-width is not one it understands - left as it is the page is laid
// out at the desktop default and then scaled down to fit.
static inline float resolvedViewportLength(float length, float deviceLength)
{
    if (length == WebCore::ViewportArguments::ValueDeviceWidth || length == WebCore::ViewportArguments::ValueDeviceHeight)
        return deviceLength;
    return length;
}

static inline NSDictionary *dictionaryForViewportArguments(const WebCore::ViewportArguments& arguments)
{
    return @{ WebViewportInitialScaleKey: @(arguments.zoom),
              WebViewportMinimumScaleKey: @(arguments.minZoom),
              WebViewportMaximumScaleKey: @(arguments.maxZoom),
              WebViewportUserScalableKey: @(arguments.userZoom),
              WebViewportShrinkToFitKey: @(0),
              WebViewportFitKey: nameForViewportFitValue(arguments.viewportFit),
              WebViewportWidthKey: @(resolvedViewportLength(arguments.width, WebCore::screenSize().width())),
              WebViewportHeightKey: @(resolvedViewportLength(arguments.height, WebCore::screenSize().height())) };
}

FloatSize WebChromeClientIOS::screenSize() const
{
    return FloatSize(WebCore::screenSize());
}

FloatSize WebChromeClientIOS::availableScreenSize() const
{
    // Upstream asserts here because its WebKit1 callers read the WAKWindow
    // directly. Driven by UIKit the viewport machinery does ask the chrome
    // client, and a zero size makes device-width resolve to the desktop default.
    return FloatSize(WebCore::availableScreenSize());
}

FloatSize WebChromeClientIOS::overrideScreenSize() const
{
    return screenSize();
}

FloatSize WebChromeClientIOS::overrideAvailableScreenSize() const
{
    return availableScreenSize();
}

void WebChromeClientIOS::dispatchViewportPropertiesDidChange(const WebCore::ViewportArguments& arguments) const
{
    NSDictionary *dictionary = dictionaryForViewportArguments(arguments);
#if defined(WEBKIT_IOS6)
    if (uiKitDelegateImplements(webView(), @selector(webView:didReceiveViewportArguments:forFrame:))) {
        [[webView() _UIKitDelegateForwarder] webView:webView() didReceiveViewportArguments:dictionary forFrame:[webView() mainFrame]];
        return;
    }
#endif
    [[webView() _UIKitDelegateForwarder] webView:webView() didReceiveViewportArguments:dictionary];
}

void WebChromeClientIOS::dispatchDisabledAdaptationsDidChange(const OptionSet<WebCore::DisabledAdaptations>&) const
{
}

void WebChromeClientIOS::notifyRevealedSelectionByScrollingFrame(WebCore::LocalFrame& frame)
{
    [[webView() _UIKitDelegateForwarder] revealedSelectionByScrollingWebFrame:kit(&frame)];
}

bool WebChromeClientIOS::isStopping()
{
    return [webView() _isStopping];
}

void WebChromeClientIOS::didLayout(LayoutType changeType)
{
    [[webView() _UIKitDelegate] webThreadWebViewDidLayout:webView() byScrolling:(changeType == ChromeClient::Scroll)];
}

void WebChromeClientIOS::didStartOverflowScroll()
{
#if defined(WEBKIT_IOS6)
    if (!uiKitDelegateImplements(webView(), @selector(webViewDidStartOverflowScroll:)))
        return;
#endif
    [[[webView() _UIKitDelegateForwarder] asyncForwarder] webViewDidStartOverflowScroll:webView()];
}

void WebChromeClientIOS::didEndOverflowScroll()
{
#if defined(WEBKIT_IOS6)
    if (!uiKitDelegateImplements(webView(), @selector(webViewDidEndOverflowScroll:)))
        return;
#endif
    [[[webView() _UIKitDelegateForwarder] asyncForwarder] webViewDidEndOverflowScroll:webView()];
}

void WebChromeClientIOS::suppressFormNotifications() 
{
    m_formNotificationSuppressions++;
}

void WebChromeClientIOS::restoreFormNotifications() 
{
    m_formNotificationSuppressions--;
    ASSERT(m_formNotificationSuppressions >= 0);
    if (m_formNotificationSuppressions < 0)
        m_formNotificationSuppressions = 0;
}

void WebChromeClientIOS::elementDidFocus(WebCore::Element& element, const WebCore::FocusOptions& options)
{
    if (m_formNotificationSuppressions > 0)
        return;

    // UIKit answers this by bringing the focused element into view, and a feed
    // that focuses something as it re-renders therefore throws the reader back
    // to the top of the document mid-scroll. Measured on the device: the offset
    // went from 870 to 0 twenty milliseconds after this message, with
    // -shouldScrollToPoint:forFrame: in between.
    //
    // Only somewhere text can be typed is worth bringing into view. A feed
    // moves focus between its own containers as it re-renders - and the gesture
    // flag is still set while that happens, so it cannot be used to tell them
    // apart - while the keyboard, which is what this message exists for, only
    // ever concerns a form control or an editable box.
    bool canBeTypedInto = is<WebCore::HTMLTextFormControlElement>(element)
        || is<WebCore::HTMLSelectElement>(element)
        || element.hasEditableStyle();
    if (!canBeTypedInto)
        return;

    [[webView() _UIKitDelegateForwarder] webView:webView() elementDidFocusNode:kit(&element)];
}

void WebChromeClientIOS::elementDidBlur(WebCore::Element& element)
{
    if (m_formNotificationSuppressions <= 0)
        [[webView() _UIKitDelegateForwarder] webView:webView() elementDidBlurNode:kit(&element)];
}

RefPtr<WebCore::PopupMenu> WebChromeClientIOS::createPopupMenu(WebCore::PopupMenuClient& client) const
{
    return adoptRef(new PopupMenuIOS(&client));
}

RefPtr<WebCore::SearchPopupMenu> WebChromeClientIOS::createSearchPopupMenu(WebCore::PopupMenuClient& client) const
{
    return adoptRef(new SearchPopupMenuIOS(&client));
}

void WebChromeClientIOS::attachRootGraphicsLayer(LocalFrame&, GraphicsLayer* graphicsLayer)
{
    // FIXME: for non-root frames we rely on RenderView positioning the root layer,
    // which is a hack. <rdar://problem/5906146>
    // Send the delegate message on the web thread to avoid <rdar://problem/8567677>
    [[webView() _UIKitDelegate] _webthread_webView:webView() attachRootLayer:graphicsLayer ? graphicsLayer->platformLayer() : 0];
}

void WebChromeClientIOS::didFlushCompositingLayers()
{
#if defined(WEBKIT_IOS6)
    callUIKitDelegateAsync(webView(), @selector(webViewDidCommitCompositingLayerChanges:));
#else
    [[[webView() _UIKitDelegateForwarder] asyncForwarder] webViewDidCommitCompositingLayerChanges:webView()];
#endif
}

bool WebChromeClientIOS::fetchCustomFixedPositionLayoutRect(IntRect& rect)
{
    NSRect updatedRect;
    if ([webView() _fetchCustomFixedPositionLayoutRect:&updatedRect]) {
        rect = enclosingIntRect(updatedRect);
        return true;
    }

    return false;
}

void WebChromeClientIOS::updateViewportConstrainedLayers(HashMap<PlatformLayer*, std::unique_ptr<ViewportConstraints>>& layerMap, const HashMap<PlatformLayer*, PlatformLayer*>& stickyContainers)
{
    [[webView() _fixedPositionContent] setViewportConstrainedLayers:layerMap stickyContainerMap:stickyContainers];
}

void WebChromeClientIOS::addOrUpdateScrollingLayer(Node* node, PlatformLayer* scrollingLayer, PlatformLayer* contentsLayer, const IntSize& scrollSize, bool allowHorizontalScrollbar, bool allowVerticalScrollbar)
{
#if defined(WEBKIT_IOS6)
    if (!uiKitDelegateImplements(webView(), @selector(webView:didCreateOrUpdateScrollingLayer:withContentsLayer:scrollSize:forNode:allowHorizontalScrollbar:allowVerticalScrollbar:)))
        return;
#endif

    DOMNode *domNode = kit(node);

    [[[webView() _UIKitDelegateForwarder] asyncForwarder] webView:webView() didCreateOrUpdateScrollingLayer:scrollingLayer withContentsLayer:contentsLayer scrollSize:[NSValue valueWithSize:scrollSize] forNode:domNode
        allowHorizontalScrollbar:allowHorizontalScrollbar allowVerticalScrollbar:allowVerticalScrollbar];
}

void WebChromeClientIOS::removeScrollingLayer(Node* node, PlatformLayer* scrollingLayer, PlatformLayer* contentsLayer)
{
#if defined(WEBKIT_IOS6)
    if (!uiKitDelegateImplements(webView(), @selector(webView:willRemoveScrollingLayer:withContentsLayer:forNode:)))
        return;
#endif

    DOMNode *domNode = kit(node);
    [[[webView() _UIKitDelegateForwarder] asyncForwarder] webView:webView() willRemoveScrollingLayer:scrollingLayer withContentsLayer:contentsLayer forNode:domNode];
}

void WebChromeClientIOS::webAppOrientationsUpdated()
{
    [[webView() _UIDelegateForwarder] webViewSupportedOrientationsUpdated:webView()];
}

void WebChromeClientIOS::focusedElementChanged(Element* element, LocalFrame*, FocusOptions, BroadcastFocusedElement)
{
    if (!is<HTMLInputElement>(element))
        return;

    HTMLInputElement& inputElement = downcast<HTMLInputElement>(*element);
    if (!inputElement.isText())
        return;

    CallFormDelegate(webView(), @selector(didFocusTextField:inFrame:), kit(&inputElement), kit(inputElement.document().frame()));
}

void WebChromeClientIOS::showPlaybackTargetPicker(bool hasVideo, WebCore::RouteSharingPolicy, const String&)
{
    CGPoint point = [[webView() _UIKitDelegateForwarder] interactionLocation];
    CGRect elementRect = [[webView() mainFrame] elementRectAtPoint:point];
    [[webView() _UIKitDelegateForwarder] showPlaybackTargetPicker:hasVideo fromRect:elementRect];
}

RefPtr<Icon> WebChromeClientIOS::createIconForFiles(const Vector<String>& filenames)
{
    return Icon::createIconForFiles(filenames);
}

#if ENABLE(ORIENTATION_EVENTS)
IntDegrees WebChromeClientIOS::deviceOrientation() const
{
    return [webView() _deviceOrientation];
}
#endif

#endif // PLATFORM(IOS_FAMILY)
