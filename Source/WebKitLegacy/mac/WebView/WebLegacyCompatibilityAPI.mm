/*
 * Copyright (C) 2005-2023 Apple Inc. All rights reserved.
 *           (C) 2006 Graham Dennis (graham.dennis@gmail.com)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#import "config.h"
#import <objc/runtime.h>


#import "WebBackForwardList.h"
#import "WebKitStatisticsPrivate.h"
#import "WebMIMETypeRegistry.h"
#import "WebCache.h"
#import "WebHTMLRepresentation.h"
#import "WebPDFViewPlaceholder.h"
#import "WebScriptObject.h"
#import "WebDataSource.h"
#import "WebHTMLView.h"
#import "WebPluginController.h"
#import "WebFixedPositionContent.h"
#import "WebHistoryItem.h"
#import "WebHistoryPrivate.h"
#import "WebVisiblePosition.h"
#import "WebCoreStatistics.h"
#import "WebFramePrivate.h"
#import "WebFrameView.h"
#import "WebPreferenceKeysPrivate.h"
#import "WebPreferencesInternal.h"
#import "WebPreferencesPrivate.h"
#import "DOMElementInternal.h"
#import "DOMNodeInternal.h"
#import "DOMRangeInternal.h"
#import "WebFrameInternal.h"
#import "WebViewInternal.h"
#import <WebCore/BackForwardCache.h>
#import <WebCore/Document.h>
#import <WebCore/FloatSize.h>
#import <WebCore/GarbageCollectionController.h>
#import <WebCore/PrintContext.h>
#import <WebCore/HTMLBodyElement.h>
#import <WebCore/HTMLDivElement.h>
#import <WebCore/HTMLElement.h>
#import <WebCore/HTMLNames.h>
#import <WebCore/LocalFrame.h>
#import <WebCore/LocalFrameInlines.h>
#import <WebCore/LocalFrameView.h>
#import <WebCore/Page.h>
#import <WebCore/Range.h>
#import <WebCore/Settings.h>
#import <WebCore/SimpleRange.h>
#import <WebCore/WebCoreThreadRun.h>
#import <WebCore/WebEventRegion.h>
#import <WebCore/markup.h>
#import <JavaScriptCore/DeleteAllCodeEffort.h>
#import <wtf/FastMalloc.h>
#import "WebViewPrivate.h"

#define WebKitNSURLDiskCacheSizePreferenceKey @"WebKitNSURLDiskCacheSize"
#define WebKitNSURLMaxRequestSizePreferenceKey @"WebKitNSURLMaxRequestSize"
#define WebKitNSURLMemoryCacheSizePreferenceKey @"WebKitNSURLMemoryCacheSize"
#define WebKitAllowCompositingLayerVisualDegradationPreferenceKey @"WebKitAllowCompositingLayerVisualDegradation"
#define WebKitAlwaysUseAcceleratedOverflowScrollPreferenceKey @"WebKitAlwaysUseAcceleratedOverflowScroll"
#define WebKitAlwaysUseBaselineOfPrimaryFontPreferenceKey @"WebKitAlwaysUseBaselineOfPrimaryFont"
#define WebKitDiskImageCacheSavedCacheDirectoryPreferenceKey @"WebKitDiskImageCacheSavedCacheDirectory"
#define WebKitForceFTPDirectoryListingsPreferenceKey @"WebKitForceFTPDirectoryListings"
#define WebKitFTPDirectoryTemplatePathPreferenceKey @"WebKitFTPDirectoryTemplatePath"
#define WebKitLayoutIntervalPreferenceKey @"WebKitLayoutInterval"
#define WebKitMaximumImageSizePreferenceKey @"WebKitMaximumImageSize"
#define WebKitObjectCacheSizePreferenceKey @"WebKitObjectCacheSize"
#define WebKitPageCacheSizePreferenceKey @"WebKitPageCacheSize"
#define WebKitUseLegacyNumberInputFieldFormattingPreferenceKey @"WebKitUseLegacyNumberInputFieldFormatting"
#define WebKitCSSCustomFilterEnabledPreferenceKey @"WebKitCSSCustomFilterEnabled"
#define WebKitCSSRegionsEnabledPreferenceKey @"WebKitCSSRegionsEnabled"
#define WebKitDiskImageCacheEnabledPreferenceKey @"WebKitDiskImageCacheEnabled"
#define WebKitDiskImageCacheMaximumCacheSizePreferenceKey @"WebKitDiskImageCacheMaximumCacheSize"
#define WebKitDiskImageCacheMinimumImageSizePreferenceKey @"WebKitDiskImageCacheMinimumImageSize"
#define WebKitEditingBehaviorPreferenceKey @"WebKitEditingBehavior"
#define WebKitFrameFlatteningEnabledPreferenceKey @"WebKitFrameFlatteningEnabled"
#define WebKitMemoryInfoEnabledPreferenceKey @"WebKitMemoryInfoEnabled"
#define WebKitPaginateDuringLayoutEnabledPreferenceKey @"WebKitPaginateDuringLayoutEnabled"
#define WebKitRegionBasedColumnsEnabledPreferenceKey @"WebKitRegionBasedColumnsEnabled"

@interface WebPreferences (WebLegacyCompatibility)
@end

@implementation WebPreferences (WebLegacyCompatibility)

- (int)_NSURLDiskCacheSize
{
    return [self _integerValueForKey:WebKitNSURLDiskCacheSizePreferenceKey];
}

- (void)_setNSURLDiskCacheSize:(int)size
{
    [self _setIntegerValue:size forKey:WebKitNSURLDiskCacheSizePreferenceKey];
}

- (int)_NSURLMaxRequestSize
{
    return [self _integerValueForKey:WebKitNSURLMaxRequestSizePreferenceKey];
}

- (void)_setNSURLMaxRequestSize:(int)size
{
    [self _setIntegerValue:size forKey:WebKitNSURLMaxRequestSizePreferenceKey];
}

- (int)_NSURLMemoryCacheSize
{
    return [self _integerValueForKey:WebKitNSURLMemoryCacheSizePreferenceKey];
}

- (void)_setNSURLMemoryCacheSize:(int)size
{
    [self _setIntegerValue:size forKey:WebKitNSURLMemoryCacheSizePreferenceKey];
}

- (BOOL)_allowCompositingLayerVisualDegradation
{
    return [self _boolValueForKey:WebKitAllowCompositingLayerVisualDegradationPreferenceKey];
}

- (void)_setAllowCompositingLayerVisualDegradation:(BOOL)flag
{
    [self _setBoolValue:flag forKey:WebKitAllowCompositingLayerVisualDegradationPreferenceKey];
}

- (BOOL)_alwaysUseAcceleratedOverflowScroll
{
    return [self _boolValueForKey:WebKitAlwaysUseAcceleratedOverflowScrollPreferenceKey];
}

- (void)_setAlwaysUseAcceleratedOverflowScroll:(BOOL)flag
{
    [self _setBoolValue:flag forKey:WebKitAlwaysUseAcceleratedOverflowScrollPreferenceKey];
}

- (BOOL)_alwaysUseBaselineOfPrimaryFont
{
    return [self _boolValueForKey:WebKitAlwaysUseBaselineOfPrimaryFontPreferenceKey];
}

- (void)_setAlwaysUseBaselineOfPrimaryFont:(BOOL)flag
{
    [self _setBoolValue:flag forKey:WebKitAlwaysUseBaselineOfPrimaryFontPreferenceKey];
}

- (NSString *)_diskImageCacheSavedCacheDirectory
{
    return [self _stringValueForKey:WebKitDiskImageCacheSavedCacheDirectoryPreferenceKey];
}

- (void)_setDiskImageCacheSavedCacheDirectory:(NSString *)directory
{
    [self _setStringValue:directory forKey:WebKitDiskImageCacheSavedCacheDirectoryPreferenceKey];
}

- (BOOL)_forceFTPDirectoryListings
{
    return [self _boolValueForKey:WebKitForceFTPDirectoryListingsPreferenceKey];
}

- (void)_setForceFTPDirectoryListings:(BOOL)flag
{
    [self _setBoolValue:flag forKey:WebKitForceFTPDirectoryListingsPreferenceKey];
}

- (NSString *)_ftpDirectoryTemplatePath
{
    return [self _stringValueForKey:WebKitFTPDirectoryTemplatePathPreferenceKey];
}

- (void)_setFTPDirectoryTemplatePath:(NSString *)path
{
    [self _setStringValue:path forKey:WebKitFTPDirectoryTemplatePathPreferenceKey];
}

- (int)_layoutInterval
{
    return [self _integerValueForKey:WebKitLayoutIntervalPreferenceKey];
}

- (void)_setLayoutInterval:(int)interval
{
    [self _setIntegerValue:interval forKey:WebKitLayoutIntervalPreferenceKey];
}

- (unsigned long)_maximumImageSize
{
    return (unsigned long)[self _longLongValueForKey:WebKitMaximumImageSizePreferenceKey];
}

- (int)_objectCacheSize
{
    return [self _integerValueForKey:WebKitObjectCacheSizePreferenceKey];
}

- (void)_setObjectCacheSize:(int)size
{
    [self _setIntegerValue:size forKey:WebKitObjectCacheSizePreferenceKey];
}

- (int)_pageCacheSize
{
    return [self _integerValueForKey:WebKitPageCacheSizePreferenceKey];
}

- (void)_setPageCacheSize:(int)size
{
    [self _setIntegerValue:size forKey:WebKitPageCacheSizePreferenceKey];
}

- (void)_setPreferenceForTestWithValue:(NSString *)value forKey:(NSString *)key
{
    [self _setStringValue:value forKey:key];
}

- (BOOL)_useLegacyNumberInputFieldFormatting
{
    return [self _boolValueForKey:WebKitUseLegacyNumberInputFieldFormattingPreferenceKey];
}

- (void)_setUseLegacyNumberInputFieldFormatting:(BOOL)flag
{
    [self _setBoolValue:flag forKey:WebKitUseLegacyNumberInputFieldFormattingPreferenceKey];
}

- (BOOL)cssCustomFilterEnabled
{
    return [self _boolValueForKey:WebKitCSSCustomFilterEnabledPreferenceKey];
}

- (void)setCSSCustomFilterEnabled:(BOOL)flag
{
    [self _setBoolValue:flag forKey:WebKitCSSCustomFilterEnabledPreferenceKey];
}

- (BOOL)cssRegionsEnabled
{
    return [self _boolValueForKey:WebKitCSSRegionsEnabledPreferenceKey];
}

- (void)setCSSRegionsEnabled:(BOOL)flag
{
    [self _setBoolValue:flag forKey:WebKitCSSRegionsEnabledPreferenceKey];
}

- (BOOL)diskImageCacheEnabled
{
    return [self _boolValueForKey:WebKitDiskImageCacheEnabledPreferenceKey];
}

- (unsigned)diskImageCacheMaximumCacheSize
{
    return (unsigned)[self _unsignedLongLongValueForKey:WebKitDiskImageCacheMaximumCacheSizePreferenceKey];
}

- (void)setDiskImageCacheMaximumCacheSize:(unsigned)size
{
    [self _setUnsignedLongLongValue:size forKey:WebKitDiskImageCacheMaximumCacheSizePreferenceKey];
}

- (unsigned)diskImageCacheMinimumImageSize
{
    return (unsigned)[self _unsignedLongLongValueForKey:WebKitDiskImageCacheMinimumImageSizePreferenceKey];
}

- (void)setDiskImageCacheMinimumImageSize:(unsigned)size
{
    [self _setUnsignedLongLongValue:size forKey:WebKitDiskImageCacheMinimumImageSizePreferenceKey];
}

- (int)editingBehavior
{
    return [self _integerValueForKey:WebKitEditingBehaviorPreferenceKey];
}

- (void)setEditingBehavior:(int)behavior
{
    [self _setIntegerValue:behavior forKey:WebKitEditingBehaviorPreferenceKey];
}

- (BOOL)isFrameFlatteningEnabled
{
    return [self _boolValueForKey:WebKitFrameFlatteningEnabledPreferenceKey];
}

- (void)setFrameFlatteningEnabled:(BOOL)flag
{
    [self _setBoolValue:flag forKey:WebKitFrameFlatteningEnabledPreferenceKey];
}

- (BOOL)memoryInfoEnabled
{
    return [self _boolValueForKey:WebKitMemoryInfoEnabledPreferenceKey];
}

- (void)setMemoryInfoEnabled:(BOOL)flag
{
    [self _setBoolValue:flag forKey:WebKitMemoryInfoEnabledPreferenceKey];
}

- (BOOL)paginateDuringLayoutEnabled
{
    return [self _boolValueForKey:WebKitPaginateDuringLayoutEnabledPreferenceKey];
}

- (void)setPaginateDuringLayoutEnabled:(BOOL)flag
{
    [self _setBoolValue:flag forKey:WebKitPaginateDuringLayoutEnabledPreferenceKey];
}

- (BOOL)regionBasedColumnsEnabled
{
    return [self _boolValueForKey:WebKitRegionBasedColumnsEnabledPreferenceKey];
}

- (void)setRegionBasedColumnsEnabled:(BOOL)flag
{
    [self _setBoolValue:flag forKey:WebKitRegionBasedColumnsEnabledPreferenceKey];
}

@end

@interface WebFrame (WebLegacyCompatibility)
@end

@implementation WebFrame (WebLegacyCompatibility)

- (int)numberOfPages:(float)pageWidth :(float)pageHeight
{
    return [self numberOfPagesWithPageWidth:pageWidth pageHeight:pageHeight];
}

- (void)printToCGContext:(CGContextRef)context :(float)pageWidth :(float)pageHeight
{
    [self printToCGContext:context pageWidth:pageWidth pageHeight:pageHeight];
}

- (NSString *)renderTreeAsExternalRepresentationForPrinting:(BOOL)forPrinting
{
    if (forPrinting)
        return [self renderTreeAsExternalRepresentationForPrinting];
    return [self renderTreeAsExternalRepresentationWithOptions:0];
}

- (void)sendOrientationChangeEvent:(int)orientation
{
    UNUSED_PARAM(orientation);
    [self deviceOrientationChanged];
}

- (NSString *)_markupStringFromRange:(DOMRange *)range nodes:(NSArray **)nodes
{
    WebCore::Range *coreRange = core(range);
    if (!coreRange) {
        if (nodes)
            *nodes = nil;
        return nil;
    }
    Vector<Ref<WebCore::Node>> nodeList;
    WTF::String markup = WebCore::serializePreservingVisualAppearance(makeSimpleRange(*coreRange), nodes ? &nodeList : nullptr, WebCore::AnnotateForInterchange::Yes);
    if (nodes) {
        NSMutableArray *array = [NSMutableArray arrayWithCapacity:nodeList.size()];
        for (auto& node : nodeList)
            [array addObject:kit(node.ptr())];
        *nodes = array;
    }
    return markup.createNSString().autorelease();
}

- (NSArray *)_nodesFromList:(void *)nodes
{
    UNUSED_PARAM(nodes);
    return nil;
}

- (unsigned)_numberOfActiveAnimations
{
    return 0;
}

- (BOOL)_pauseAnimation:(NSString *)name onNode:(DOMNode *)node atTime:(double)time
{
    UNUSED_PARAM(name);
    UNUSED_PARAM(node);
    UNUSED_PARAM(time);
    return NO;
}

- (BOOL)_pauseTransitionOfProperty:(NSString *)name onNode:(DOMNode *)node atTime:(double)time
{
    UNUSED_PARAM(name);
    UNUSED_PARAM(node);
    UNUSED_PARAM(time);
    return NO;
}

- (void)_suspendAnimations
{
    if (auto page = [[self webView] page])
        page->suspendActiveDOMObjectsAndAnimations();
}

- (void)_resumeAnimations
{
    if (auto page = [[self webView] page])
        page->resumeActiveDOMObjectsAndAnimations();
}

- (void)_setExcludeFromTextSearch:(bool)exclude
{
    UNUSED_PARAM(exclude);
}

- (void)_setIsDisconnected:(bool)disconnected
{
    UNUSED_PARAM(disconnected);
}

- (BOOL)_shouldFlattenCompositingLayers:(CGContextRef)context
{
    UNUSED_PARAM(context);
    return NO;
}

- (DOMRange *)_smartDeleteRangeForProposedRange:(DOMRange *)proposedRange
{
    return proposedRange;
}

- (NSString *)_stringWithDocumentTypeStringAndMarkupString:(NSString *)markupString
{
    auto* frame = core(self);
    if (!frame)
        return markupString;
    RefPtr document = frame->document();
    if (!document)
        return markupString;
    NSString *doctype = WebCore::documentTypeString(*document).createNSString().autorelease();
    return [doctype stringByAppendingString:markupString ?: @""];
}

- (void)clearPPTStats
{
}

- (void)getPPTStatsWithParseCount:(unsigned *)parseCount withLayoutCount:(unsigned *)layoutCount withForcedLayoutCount:(unsigned *)forcedLayoutCount withParseDuration:(double *)parseDuration withLayoutDuration:(double *)layoutDuration
{
    if (parseCount)
        *parseCount = 0;
    if (layoutCount)
        *layoutCount = 0;
    if (forcedLayoutCount)
        *forcedLayoutCount = 0;
    if (parseDuration)
        *parseDuration = 0;
    if (layoutDuration)
        *layoutDuration = 0;
}

- (NSString *)counterValueForElement:(DOMElement *)element
{
    UNUSED_PARAM(element);
    return nil;
}

- (void)createDefaultFieldEditorDocumentStructure
{
    auto* frame = core(self);
    if (!frame)
        return;
    RefPtr document = frame->document();
    if (!document)
        return;
    RefPtr<WebCore::HTMLElement> body = document->body();
    if (!body)
        return;

    body->setAttributeWithoutSynchronization(WebCore::HTMLNames::styleAttr, "margin:0; padding:0; border:0;"_s);

    auto appendDiv = [&](const AtomString& identifier, const AtomString& inlineStyle, bool editable) {
        auto div = WebCore::HTMLDivElement::create(*document);
        div->setAttributeWithoutSynchronization(WebCore::HTMLNames::idAttr, identifier);
        if (!inlineStyle.isEmpty())
            div->setAttributeWithoutSynchronization(WebCore::HTMLNames::styleAttr, inlineStyle);
        if (editable)
            div->setAttributeWithoutSynchronization(WebCore::HTMLNames::contenteditableAttr, "true"_s);
        body->appendChild(div);
    };

    appendDiv("text"_s, "white-space:pre; overflow:hidden; -webkit-user-select:text; word-wrap:normal;"_s, true);
    appendDiv("size"_s, "position:absolute; visibility:hidden; white-space:pre; top:0; left:0;"_s, false);
}

- (void)finalize
{
}

- (unsigned)formElementsCharacterCount
{
    return 0;
}

- (CGImageRef)imageForNode:(DOMNode *)node allowDownsampling:(BOOL)allowDownsampling drawContentBehindTransparentNodes:(BOOL)drawContentBehind
{
    UNUSED_PARAM(node);
    UNUSED_PARAM(allowDownsampling);
    UNUSED_PARAM(drawContentBehind);
    return nullptr;
}

- (bool)isPageBoxVisible:(int)pageIndex
{
    return WebCore::PrintContext::isPageBoxVisible(core(self), pageIndex);
}

- (BOOL)isSingleLine
{
    return NO;
}

- (void)setIsSingleLine:(BOOL)singleLine
{
    UNUSED_PARAM(singleLine);
}

- (BOOL)mediaDataLoadsAutomatically
{
    return YES;
}

- (void)setMediaDataLoadsAutomatically:(BOOL)loadsAutomatically
{
    UNUSED_PARAM(loadsAutomatically);
}

- (id )nextUnperturbedDictationResultBoundaryFromPosition:(id )position
{
    return position;
}

- (id )previousUnperturbedDictationResultBoundaryFromPosition:(id )position
{
    return position;
}

- (int)pageNumberForElement:(DOMElement *)element :(float)pageWidth :(float)pageHeight
{
    return WebCore::PrintContext::pageNumberForElement(core(element), WebCore::FloatSize(pageWidth, pageHeight));
}

- (NSString *)pageProperty:(const char *)propertyName :(int)pageNumber
{
    return WebCore::PrintContext::pageProperty(core(self), WTF::String::fromUTF8(propertyName), pageNumber).createNSString().autorelease();
}

- (NSString *)pageSizeAndMarginsInPixels:(int)pageIndex :(int)width :(int)height :(int)marginTop :(int)marginRight :(int)marginBottom :(int)marginLeft
{
    return WebCore::PrintContext::pageSizeAndMarginsInPixels(core(self), pageIndex, width, height, marginTop, marginRight, marginBottom, marginLeft).createNSString().autorelease();
}

@end

@interface WebView (WebLegacyCompatibility)
@end

@implementation WebView (WebLegacyCompatibility)

+ (void)_addOriginAccessWhitelistEntryWithSourceOrigin:(NSString *)sourceOrigin destinationProtocol:(NSString *)destinationProtocol destinationHost:(NSString *)destinationHost allowDestinationSubdomains:(BOOL)allowDestinationSubdomains
{
    [self _addOriginAccessAllowListEntryWithSourceOrigin:sourceOrigin destinationProtocol:destinationProtocol destinationHost:destinationHost allowDestinationSubdomains:allowDestinationSubdomains];
}

+ (void)_removeOriginAccessWhitelistEntryWithSourceOrigin:(NSString *)sourceOrigin destinationProtocol:(NSString *)destinationProtocol destinationHost:(NSString *)destinationHost allowDestinationSubdomains:(BOOL)allowDestinationSubdomains
{
    [self _removeOriginAccessAllowListEntryWithSourceOrigin:sourceOrigin destinationProtocol:destinationProtocol destinationHost:destinationHost allowDestinationSubdomains:allowDestinationSubdomains];
}

+ (void)_resetOriginAccessWhitelists
{
    [self _resetOriginAccessAllowLists];
}

+ (void)_addUserScriptToGroup:(NSString *)groupName world:(WebScriptWorld *)world source:(NSString *)source url:(NSURL *)url whitelist:(NSArray *)whitelist blacklist:(NSArray *)blacklist injectionTime:(WebUserScriptInjectionTime)injectionTime
{
    [self _addUserScriptToGroup:groupName world:world source:source url:url includeMatchPatternStrings:whitelist excludeMatchPatternStrings:blacklist injectionTime:injectionTime injectedFrames:WebInjectInAllFrames];
}

+ (void)_addUserScriptToGroup:(NSString *)groupName world:(WebScriptWorld *)world source:(NSString *)source url:(NSURL *)url whitelist:(NSArray *)whitelist blacklist:(NSArray *)blacklist injectionTime:(WebUserScriptInjectionTime)injectionTime injectedFrames:(WebUserContentInjectedFrames)injectedFrames
{
    [self _addUserScriptToGroup:groupName world:world source:source url:url includeMatchPatternStrings:whitelist excludeMatchPatternStrings:blacklist injectionTime:injectionTime injectedFrames:injectedFrames];
}

+ (void)_addUserStyleSheetToGroup:(NSString *)groupName world:(WebScriptWorld *)world source:(NSString *)source url:(NSURL *)url whitelist:(NSArray *)whitelist blacklist:(NSArray *)blacklist
{
    [self _addUserStyleSheetToGroup:groupName world:world source:source url:url includeMatchPatternStrings:whitelist excludeMatchPatternStrings:blacklist injectedFrames:WebInjectInAllFrames];
}

+ (void)_addUserStyleSheetToGroup:(NSString *)groupName world:(WebScriptWorld *)world source:(NSString *)source url:(NSURL *)url whitelist:(NSArray *)whitelist blacklist:(NSArray *)blacklist injectedFrames:(WebUserContentInjectedFrames)injectedFrames
{
    [self _addUserStyleSheetToGroup:groupName world:world source:source url:url includeMatchPatternStrings:whitelist excludeMatchPatternStrings:blacklist injectedFrames:injectedFrames];
}

+ (NSString *)_standardUserAgentWithApplicationName:(NSString *)applicationName osMarketingVersion:(NSString *)osMarketingVersion
{
    UNUSED_PARAM(osMarketingVersion);
    return [self _standardUserAgentWithApplicationName:applicationName];
}

+ (void)garbageCollectNow
{
    [WebCoreStatistics garbageCollectJavaScriptObjects];
}

+ (void)purgeInactiveFontData
{
    [WebCoreStatistics purgeInactiveFontData];
}

+ (void)_handleMemoryWarning
{
    [WebCache empty];
    [WebCoreStatistics garbageCollectJavaScriptObjects];
}

+ (void)discardAllCompiledCode
{
    WebThreadRun(^{
        WebCore::GarbageCollectionController::singleton().deleteAllCode(JSC::DeleteAllCodeIfNotCollecting);
    });
}

+ (void)releaseFastMallocMemory
{
    WebThreadRun(^{
        WTF::releaseFastMallocFreeMemory();
    });
}

+ (void)drainLayerPool
{
}

+ (void)registerForMemoryNotifications
{
}

+ (BOOL)_acceleratedImageDecoding
{
    return NO;
}

+ (void)_setAcceleratedImageDecoding:(BOOL)enabled
{
    UNUSED_PARAM(enabled);
}

+ (BOOL)_allowCookies
{
    return YES;
}

+ (void)_setAllowCookies:(BOOL)allow
{
    UNUSED_PARAM(allow);
}

+ (BOOL)_allowsRoundingHacks
{
    return NO;
}

+ (void)_setAllowsRoundingHacks:(BOOL)allow
{
    UNUSED_PARAM(allow);
}

+ (double)_defaultMinimumTimerInterval
{
    return 0.004;
}

+ (unsigned)_maximumImageSizeBeforeSubsampling
{
    return 0;
}

+ (void)_setMaximumImageSizeBeforeSubsampling:(unsigned)size
{
    UNUSED_PARAM(size);
}

+ (BOOL)_shouldUseFontSmoothing
{
    return NO;
}

+ (void)_setShouldUseFontSmoothing:(BOOL)smooth
{
    UNUSED_PARAM(smooth);
}

+ (id)sharedWebInspectorServer
{
    return nil;
}

- (id)_initWithFrame:(CGRect)frame frameName:(NSString *)frameName groupName:(NSString *)groupName usesDocumentViews:(BOOL)usesDocumentViews
{
    UNUSED_PARAM(usesDocumentViews);
    return [self _initWithFrame:frame frameName:frameName groupName:groupName];
}

- (void)_setGlobalHistoryItem:(void *)item
{
    UNUSED_PARAM(item);
}

- (BOOL)_syncCompositingChanges
{
    return [self _flushCompositingChanges];
}

- (void)_scheduleCompositingLayerSync
{
    [self _scheduleUpdateRendering];
}

- (void)_geolocationDidFailWithError:(NSError *)error
{
    [self _geolocationDidFailWithMessage:[error localizedDescription]];
}

- (BOOL)_catchesDelegateExceptions
{
    return YES;
}

- (void)_setCatchesDelegateExceptions:(BOOL)catches
{
    UNUSED_PARAM(catches);
}

- (void)_clearBackForwardCache
{
    WebThreadRun(^{
        WebCore::BackForwardCache::singleton().pruneToSizeNow(0, WebCore::PruningReason::MemoryPressure);
    });
}

- (JSValueRef)_computedStyleIncludingVisitedInfo:(JSContextRef)context forElement:(JSValueRef)element
{
    UNUSED_PARAM(context);
    UNUSED_PARAM(element);
    return nullptr;
}

- (id)_globalHistoryItem
{
    return nil;
}

- (BOOL)_inViewSourceMode
{
    return NO;
}

- (void)_setInViewSourceMode:(BOOL)viewSourceMode
{
    UNUSED_PARAM(viewSourceMode);
}

- (BOOL)_includesFlattenedCompositingLayersWhenDrawingToBitmap
{
    return NO;
}

- (void)_setIncludesFlattenedCompositingLayersWhenDrawingToBitmap:(BOOL)includes
{
    UNUSED_PARAM(includes);
}

- (BOOL)_needsPreHTML5ParserQuirks
{
    return NO;
}

- (BOOL)_needsUnrestrictedGetMatchedCSSRules
{
    return NO;
}

- (void)_notificationControllerDestroyed
{
}

- (void)_setCustomHTMLTokenizerChunkSize:(int)chunkSize
{
    UNUSED_PARAM(chunkSize);
}

- (void)_setCustomHTMLTokenizerTimeDelay:(double)timeDelay
{
    UNUSED_PARAM(timeDelay);
}

- (void)_setJavaScriptURLsAreAllowed:(BOOL)areAllowed
{
    UNUSED_PARAM(areAllowed);
}

- (void)_setMinimumTimerInterval:(double)interval
{
    if (auto* frame = core([self mainFrame]))
        frame->settings().setMinimumDOMTimerInterval(WTF::Seconds(interval));
}

- (void)_setNetworkStateIsOnline:(BOOL)isOnline
{
    UNUSED_PARAM(isOnline);
}

- (NSArray *)_touchEventRegions
{
    // Reporting a region here makes UIKit stop delivering mouse events without
    // starting to deliver touch ones, so the page ends up receiving nothing at
    // all. Measured on the device; left empty until that path is understood.
    return nil;
}

- (id)_videoProxyPluginForMIMEType:(NSString *)mimeType
{
    UNUSED_PARAM(mimeType);
    return nil;
}

- (void)_viewWillDrawInternal
{
}

- (BOOL)canBeRemotelyInspected
{
    return NO;
}

- (void)setIndicatingForRemoteInspector:(BOOL)indicating
{
    UNUSED_PARAM(indicating);
}

- (id)remoteInspectorUserInfo
{
    return nil;
}

- (void)setRemoteInspectorUserInfo:(id)userInfo
{
    UNUSED_PARAM(userInfo);
}

- (BOOL)cssAnimationsSuspended
{
    return NO;
}

- (void)setCSSAnimationsSuspended:(BOOL)suspended
{
    UNUSED_PARAM(suspended);
}

- (void)finalize
{
}

- (NSString *)hostApplicationBundleId
{
    return [[NSBundle mainBundle] bundleIdentifier];
}

- (NSString *)hostApplicationName
{
    return [[NSProcessInfo processInfo] processName];
}

- (void)setHostApplicationBundleId:(NSString *)bundleId name:(NSString *)name
{
    UNUSED_PARAM(bundleId);
    UNUSED_PARAM(name);
}

@end

@interface WebFrameView (WebLegacyCompatibility)
@end

@implementation WebFrameView (WebLegacyCompatibility)

- (void)finalize
{
}

@end

@interface WebApplicationCache : NSObject
@end

@implementation WebApplicationCache

+ (void)initializeWithBundleIdentifier:(NSString *)bundleIdentifier
{
    UNUSED_PARAM(bundleIdentifier);
}

+ (long long)maximumSize
{
    return 0;
}

+ (void)setMaximumSize:(long long)size
{
    UNUSED_PARAM(size);
}

+ (long long)defaultOriginQuota
{
    return 0;
}

+ (void)setDefaultOriginQuota:(long long)quota
{
    UNUSED_PARAM(quota);
}

+ (long long)diskUsageForOrigin:(id)origin
{
    UNUSED_PARAM(origin);
    return 0;
}

+ (void)deleteAllApplicationCaches
{
}

+ (void)deleteCacheForOrigin:(id)origin
{
    UNUSED_PARAM(origin);
}

+ (NSArray *)originsWithCache
{
    return @[];
}

@end

@interface WebFixedPositionContent (WebLegacyCompatibility)
@end

@implementation WebFixedPositionContent (WebLegacyCompatibility)

- (BOOL)hasFixedPositionLayers
{
    return [self hasFixedOrStickyPositionLayers];
}

- (void)lockLayers
{
}

- (void)unlockLayers
{
}

- (void)removeAllLayers
{
    [self setViewportConstrainedLayers:nil stickyContainerMap:nil];
}

- (void)removeLayer:(CALayer *)layer insideLayerSync:(BOOL)insideLayerSync
{
    UNUSED_PARAM(layer);
    UNUSED_PARAM(insideLayerSync);
}

- (void)addOrUpdateLayer:(CALayer *)layer viewportConstraints:(void *)constraints insideLayerSync:(BOOL)insideLayerSync
{
    UNUSED_PARAM(layer);
    UNUSED_PARAM(constraints);
    UNUSED_PARAM(insideLayerSync);
}

@end

@interface WebHistoryItem (WebLegacyCompatibility)
@end

@implementation WebHistoryItem (WebLegacyCompatibility)

+ (void)initWindowWatcherIfNecessary
{
}

- (id)initWithURL:(NSURL *)url target:(NSString *)target parent:(NSString *)parent title:(NSString *)title
{
    UNUSED_PARAM(target);
    UNUSED_PARAM(parent);
    return [self initWithURLString:[url absoluteString] title:title lastVisitedTimeInterval:0];
}

- (id)targetItem
{
    return nil;
}

- (int)visitCount
{
    return 0;
}

- (void)setVisitCount:(int)count
{
    UNUSED_PARAM(count);
}

- (void)setAlwaysAttemptToUsePageCache:(BOOL)attempt
{
    UNUSED_PARAM(attempt);
}

- (BOOL)_lastVisitWasHTTPNonGet
{
    return NO;
}

- (void)_setLastVisitWasFailure:(BOOL)failure
{
    UNUSED_PARAM(failure);
}

- (NSDate *)_lastVisitedDate
{
    return nil;
}

- (void)_setLastVisitedTimeInterval:(NSTimeInterval)interval
{
    UNUSED_PARAM(interval);
}

- (void)_recordInitialVisit
{
}

- (void)_visitedWithTitle:(NSString *)title increaseVisitCount:(BOOL)increaseVisitCount
{
    UNUSED_PARAM(title);
    UNUSED_PARAM(increaseVisitCount);
}

- (void)_mergeAutoCompleteHints:(WebHistoryItem *)item
{
    UNUSED_PARAM(item);
}

- (unsigned long)_getDailyVisitCounts:(const int **)counts
{
    if (counts)
        *counts = nullptr;
    return 0;
}

- (unsigned long)_getWeeklyVisitCounts:(const int **)counts
{
    if (counts)
        *counts = nullptr;
    return 0;
}

- (void)_setTransientProperty:(id)property forKey:(NSString *)key
{
    UNUSED_PARAM(property);
    UNUSED_PARAM(key);
}

- (id)_transientPropertyForKey:(NSString *)key
{
    UNUSED_PARAM(key);
    return nil;
}

- (void)finalize
{
}

@end

@interface WebHistory (WebLegacyCompatibility)
@end

@implementation WebHistory (WebLegacyCompatibility)

- (void)_addVisitedLinksToPageGroup:(void *)group
{
    UNUSED_PARAM(group);
}

- (void)_visitedURL:(NSURL *)url withTitle:(NSString *)title method:(NSString *)method wasFailure:(BOOL)wasFailure increaseVisitCount:(BOOL)increaseVisitCount
{
    UNUSED_PARAM(method);
    UNUSED_PARAM(wasFailure);
    UNUSED_PARAM(increaseVisitCount);
    if (url)
        [self addItems:@[[[[WebHistoryItem alloc] initWithURLString:[url absoluteString] title:title lastVisitedTimeInterval:[NSDate timeIntervalSinceReferenceDate]] autorelease]]];
}

- (void)finalize
{
}

@end

@interface WebVisiblePosition (WebLegacyCompatibility)
@end

@implementation WebVisiblePosition (WebLegacyCompatibility)

// iOS 6 asked for the next boundary with one selector per granularity; 2.54
// takes the granularity as an argument. Same operation, so these forward to it
// rather than answering with the position they were given, which would leave
// UIKit unable to move the caret at all.

- (WebVisiblePosition *)nextCharacterBoundaryInDirection:(WebTextAdjustmentDirection)direction
{
    return [self positionOfNextBoundaryOfGranularity:WebTextGranularityCharacter inDirection:direction];
}

- (WebVisiblePosition *)nextWordBoundaryInDirection:(WebTextAdjustmentDirection)direction
{
    return [self positionOfNextBoundaryOfGranularity:WebTextGranularityWord inDirection:direction];
}

- (WebVisiblePosition *)nextSentenceBoundaryInDirection:(WebTextAdjustmentDirection)direction
{
    return [self positionOfNextBoundaryOfGranularity:WebTextGranularitySentence inDirection:direction];
}

- (WebVisiblePosition *)nextLineBoundaryInDirection:(WebTextAdjustmentDirection)direction
{
    return [self positionOfNextBoundaryOfGranularity:WebTextGranularityLine inDirection:direction];
}

- (WebVisiblePosition *)nextParagraphBoundaryInDirection:(WebTextAdjustmentDirection)direction
{
    return [self positionOfNextBoundaryOfGranularity:WebTextGranularityParagraph inDirection:direction];
}

- (WebVisiblePosition *)nextDocumentBoundaryInDirection:(WebTextAdjustmentDirection)direction
{
    return [self positionOfNextBoundaryOfGranularity:WebTextGranularityAll inDirection:direction];
}

@end

@interface WebHTMLView (WebLegacyCompatibility)
@end

@implementation WebHTMLView (WebLegacyCompatibility)

- (BOOL)_hasHTMLDocument
{
    return YES;
}

- (BOOL)_insideAnotherHTMLView
{
    return NO;
}

- (BOOL)_transparentBackground
{
    return NO;
}

- (void)_setTransparentBackground:(BOOL)transparent
{
    UNUSED_PARAM(transparent);
}

- (BOOL)_web_isDrawingIntoLayer
{
    return NO;
}

- (void)_autoscroll
{
}

- (void)_startAutoscrollTimer:(id)event
{
    UNUSED_PARAM(event);
}

- (void)_clearLastHitViewIfSelf
{
}

- (void)_updateControlTints
{
}

- (void)_updateSelectionForInputManager
{
}

- (void)_windowChangedKeyState
{
}

- (BOOL)_shouldDeleteRange:(id)range
{
    UNUSED_PARAM(range);
    return YES;
}

- (id)_highlighterForType:(NSString *)type
{
    UNUSED_PARAM(type);
    return nil;
}

- (void)_setHighlighter:(id)highlighter ofType:(NSString *)type
{
    UNUSED_PARAM(highlighter);
    UNUSED_PARAM(type);
}

- (void)_removeHighlighterOfType:(NSString *)type
{
    UNUSED_PARAM(type);
}

- (void)_web_makePluginSubviewsPerformSelector:(SEL)selector withObject:(id)object
{
    UNUSED_PARAM(selector);
    UNUSED_PARAM(object);
}

- (void)attachRootLayer:(CALayer *)layer
{
    UNUSED_PARAM(layer);
}

- (void)detachRootLayer
{
}

- (void)drawLayer:(CALayer *)layer inContext:(CGContextRef)context
{
    UNUSED_PARAM(layer);
    UNUSED_PARAM(context);
}

- (void)layoutToMinimumPageWidth:(float)minimumPageWidth height:(float)height originalPageWidth:(float)originalPageWidth originalPageHeight:(float)originalPageHeight maximumShrinkRatio:(float)maximumShrinkRatio adjustingViewSize:(BOOL)adjustingViewSize
{
    UNUSED_PARAM(minimumPageWidth);
    UNUSED_PARAM(height);
    UNUSED_PARAM(originalPageWidth);
    UNUSED_PARAM(originalPageHeight);
    UNUSED_PARAM(maximumShrinkRatio);
    UNUSED_PARAM(adjustingViewSize);
}

- (void)finalize
{
}

@end

@interface WebPluginController (WebLegacyCompatibility)
@end

@implementation WebPluginController (WebLegacyCompatibility)

+ (id)plugInViewWithArguments:(NSDictionary *)arguments fromPluginPackage:(id)package
{
    UNUSED_PARAM(arguments);
    UNUSED_PARAM(package);
    return nil;
}

+ (void)pluginViewHidden:(id)view
{
    UNUSED_PARAM(view);
}

- (void)pluginViewCreated:(id)view
{
    UNUSED_PARAM(view);
}

- (void)_webPluginContainerPostMediaPlayerNotification:(int)notification forElement:(id)element
{
    UNUSED_PARAM(notification);
    UNUSED_PARAM(element);
}

- (void)_webPluginContainerSetMediaPlayerProxy:(id)proxy forElement:(id)element
{
    UNUSED_PARAM(proxy);
    UNUSED_PARAM(element);
}

@end

@interface WebCache (WebLegacyCompatibility)
@end

@implementation WebCache (WebLegacyCompatibility)

+ (bool)addImageToCache:(CGImageRef)image forURL:(NSURL *)url
{
    UNUSED_PARAM(image);
    UNUSED_PARAM(url);
    return false;
}

+ (void)removeImageFromCacheForURL:(NSURL *)url
{
    UNUSED_PARAM(url);
}

@end

@interface WebDataSource (WebLegacyCompatibility)
@end

@implementation WebDataSource (WebLegacyCompatibility)

- (BOOL)_transferApplicationCache:(NSString *)destination
{
    UNUSED_PARAM(destination);
    return NO;
}

- (void)finalize
{
}

@end

@interface WebBackForwardList (WebLegacyCompatibility)
@end

@implementation WebBackForwardList (WebLegacyCompatibility)

- (void)finalize
{
}

@end

@interface WebHTMLRepresentation (WebLegacyCompatibility)
@end

@implementation WebHTMLRepresentation (WebLegacyCompatibility)

- (void)finalize
{
}

@end

@interface WebScriptObject (WebLegacyCompatibility)
@end

@implementation WebScriptObject (WebLegacyCompatibility)

- (void)finalize
{
}

@end

@interface WebPDFViewPlaceholder (WebLegacyCompatibility)
@end

@implementation WebPDFViewPlaceholder (WebLegacyCompatibility)

- (void)dataSourceMemoryMapped
{
}

- (void)dataSourceMemoryMapFailed
{
}

@end

@interface WebMIMETypeRegistry (WebLegacyCompatibility)
@end

@implementation WebMIMETypeRegistry (WebLegacyCompatibility)

+ (void)initialize
{
}

@end

@interface WebView (WebLegacyKeyboardInput)
@end

@implementation WebView (WebLegacyKeyboardInput)

// The keyboard on iOS does the inserting: WebKit hands it the typed characters
// and the keyboard calls back with -insertText:. iOS 6's UIWebDocumentView
// spells those two entry points -addInputString: and -deleteFromInput, without
// the flags argument later versions added, so calling only the newer names
// sends every keystroke to a selector that does not exist there - the event is
// marked handled and the character is silently dropped. Ask the delegate which
// spelling it has; the forwarder answers respondsToSelector: for everything.
- (void)_sendInputString:(NSString *)string withFlags:(unsigned)flags fromVariantKey:(BOOL)fromVariantKey
{
    id delegate = [self _UIKitDelegate];
    id forwarder = [self _UIKitDelegateForwarder];

    if ([delegate respondsToSelector:@selector(addInputString:withFlags:)])
        [forwarder addInputString:string withFlags:flags];
    else if ([delegate respondsToSelector:@selector(addInputString:fromVariantKey:)])
        [forwarder addInputString:string fromVariantKey:fromVariantKey];
    else if ([delegate respondsToSelector:@selector(addInputString:)])
        [forwarder addInputString:string];
    else if ([delegate respondsToSelector:@selector(insertText:)])
        [forwarder insertText:string];
}

- (void)_sendDeleteFromInputWithFlags:(unsigned)flags
{
    id delegate = [self _UIKitDelegate];
    id forwarder = [self _UIKitDelegateForwarder];

    if ([delegate respondsToSelector:@selector(deleteFromInputWithFlags:)])
        [forwarder deleteFromInputWithFlags:flags];
    else if ([delegate respondsToSelector:@selector(deleteFromInput)])
        [forwarder deleteFromInput];
}

@end
