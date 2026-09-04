/*
 * Copyright (C) 2011, 2014 Apple Inc. All rights reserved.
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
#include <wtf/MemoryFootprint.h>
#include "MemoryRelease.h"

#if defined(WEBKIT_IOS6)
#include <mach/mach.h>
#include <mach/task.h>
#include <stdlib.h>
#endif

#include "AsyncNodeDeletionQueueInlines.h"
#include "BackForwardCache.h"
#include "CSSFontSelector.h"
#include "CSSValuePool.h"
#include "Chrome.h"
#include "ChromeClient.h"
#include "CommonVM.h"
#include "CookieJar.h"
#include "DocumentResourceLoader.h"
#include "DocumentView.h"
#include "FontCache.h"
#include "GarbageCollectionController.h"
#include "HRTFElevation.h"
#include "HTMLMediaElement.h"
#include "HTMLNameCache.h"
#include "ImmutableStyleProperties.h"
#include "InlineStyleSheetOwner.h"
#include "InspectorInstrumentation.h"
#include "LayoutIntegrationLineLayout.h"
#include "LocalFrame.h"
#include "Logging.h"
#include "MemoryCache.h"
#include "Page.h"
#include "PerformanceLogging.h"
#include "PlatformRenderTheme.h"
#include "PluginDocument.h"
#include "RenderObjectInlines.h"
#include "RenderTheme.h"
#include "RenderView.h"
#include "SVGPathElement.h"
#include "ScrollingThread.h"
#include "SelectorChecker.h"
#include "SelectorQuery.h"
#include "StyleDocumentScope.h"
#include "StyleSheetContentsCache.h"
#include "StyledElement.h"
#include "TextBreakingPositionCache.h"
#include "TextPainter.h"
#include "WorkerGlobalScope.h"
#include "WorkerThread.h"
#include <JavaScriptCore/VM.h>
#include <wtf/ResourceUsage.h>
#include <wtf/SystemTracing.h>
#include <wtf/text/MakeString.h>

#if PLATFORM(COCOA)
#include "ResourceUsageThread.h"
#include <wtf/spi/darwin/OSVariantSPI.h>
#endif

#if ENABLE(INTERACTION_REGIONS_IN_EVENT_REGION)
#include "InteractionRegion.h"
#endif

namespace WebCore {

#if defined(WEBKIT_IOS6)
static double residentMegabytes()
{
    struct task_basic_info info;
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return info.resident_size / 1048576.0;
}

// This is a last resort, not a policy, and it has been set wrongly in both directions.
//
// It was 235 when the process ran at 200-290 MB. It was then lowered to 160 on the argument
// that 235 was above every valid measurement and therefore dead - true of the numbers at
// that moment, and wrong within hours, because the process had since grown to 204-244 MB.
// At 160 the test is true on every firing of the application's memory valve, and a census
// on the device found compiled code being discarded in full twice per 160-second scroll.
//
// That single constant produced four separate symptoms elsewhere: 28,456 inline-cache
// condition-set allocations in 100 s (68% of all C++ allocation on the web thread) as the
// caches were rebuilt from nothing; a recorded tier-up conclusion whose stated reason turned
// out to measure this instead (compiling the whole interpreted population costs ~2 MB of a
// 27.2 MB pool, not a memory blow-up); the ratchet in CodeBlock::~CodeBlock, which sets
// didOptimize = False on a baseline block that never reached the DFG and quadruples that
// function's next threshold; and continuous traffic through the single process-wide
// executable-allocator lock, taken while CodeBlock::m_lock is held.
//
// Deleting code was measured expensive long before any of that: 71-77 s of stalled time over
// four page switches with the code deleted against 51-53 s with it kept.
//
// The deeper fault was not the number but the unit. This gate had its own residentMegabytes()
// reading task_basic_info.resident_size, while WTF::memoryFootprint() - which the collector's
// bands are calibrated against - was changed to report the task's own anonymous memory,
// because resident size is three-fifths shared cache and framework text that collecting or
// discarding code cannot release. Two related decisions were left on two different scales.
// Restoring 235 on the resident scale would not have fixed it either: resident peaks at 249.
//
// So this now asks the same question the collector asks, and fires only when the collector
// has already lost - the collector's own hard band (JSC_IOS6_GC_HARD_MB, the one that
// promotes a collection to a full one) is 180 MB of the same quantity, not 130; 130 was true
// when this paragraph was written and is not true now.
//
// The number this gate actually sat against was never the hard band, it was the collector's
// absolute band (JSC_IOS6_GC_ABSOLUTE_MB) - the valve that force-releases full-collection
// suppression. 235 here vs 225 there was a 10 MB margin: fire just after the collector's own
// last resort has already been spent, not instead of it.
//
// Tonight JSC_IOS6_GC_ABSOLUTE_MB moved 225 -> 265, on live-device evidence that 225 sat
// inside ordinary navigation load and kept force-releasing suppression that didn't need
// releasing. This gate did not move with it, which inverts the ordering it was built on:
// at 235 it now fires 30 MB before the collector's own last resort does, on the same kind of
// ordinary load, not after the collector has already lost. The standing resident measurement
// already on record for this device - 205-235 MB in the first hundred seconds, peaking at 249
// - sits on top of 235, not above it, so this is not a hypothetical crossing.
//
// Moved to 275, keeping the same 10 MB margin above the new absolute band. Reasoned from the
// in-tree numbers, not re-measured live tonight - if a live census shows resident still
// routinely clearing 275, that call was wrong and needs a device run, not another guess.
// WEBKIT_IOS6_CODE_DELETION_THRESHOLD_MB moves it.
static double codeDeletionThresholdMegabytes()
{
    static const double threshold = [] -> double {
        if (const char* override = getenv("WEBKIT_IOS6_CODE_DELETION_THRESHOLD_MB")) {
            double value = atof(override);
            if (value > 0)
                return value;
        }
        return 275;
    }();
    return threshold;
}

bool shouldDeleteAllCodeForMemoryPressure()
{
    return residentMegabytes() >= codeDeletionThresholdMegabytes();
}
#endif

static void releaseNoncriticalMemory(MaintainMemoryCache maintainMemoryCache)
{
    RenderTheme::singleton().purgeCaches();

    FontCache::releaseNoncriticalMemoryInAllFontCaches();

    GlyphDisplayListCache::singleton().clear();
    SelectorQueryCache::singleton().clear();

    auto allDocuments = Document::allDocuments();
    auto protectedDocuments = WTF::map(allDocuments, [](auto& document) -> Ref<Document> {
        return document.get();
    });

    for (auto& document : protectedDocuments) {
        document->asyncNodeDeletionQueue().deleteNodesNow();
        if (CheckedPtr renderView = document->renderView()) {
            LayoutIntegration::LineLayout::releaseCaches(*renderView);
            Layout::TextBreakingPositionCache::singleton().clear();
            renderView->layoutContext().deleteDetachedRenderersNow();
            renderView->layoutContext().deleteDetachedInlineContentNow();
        }
    }

    if (maintainMemoryCache == MaintainMemoryCache::No)
        MemoryCache::singleton().pruneDeadResourcesToSize(0);

    Style::StyleSheetContentsCache::singleton().clear();
    HTMLNameCache::clear();
    ImmutableStyleProperties::clearDeduplicationMap();
    SelectorChecker::clearCompiledHasArgumentSelectors();
#if !defined(WEBKIT_IOS6)
    SVGPathElement::clearCache();
#endif
#if ENABLE(INTERACTION_REGIONS_IN_EVENT_REGION)
    InteractionRegion::clearCache();
#endif
}

static void releaseCriticalMemory(Synchronous synchronous, MaintainBackForwardCache maintainBackForwardCache, MaintainMemoryCache maintainMemoryCache)
{
    // Right now, the only reason we call release critical memory while not under memory pressure is if the process is about to be suspended.
    if (maintainBackForwardCache == MaintainBackForwardCache::No) {
        PruningReason pruningReason = MemoryPressureHandler::singleton().isUnderMemoryPressure() ? PruningReason::MemoryPressure : PruningReason::ProcessSuspended;
        BackForwardCache::singleton().pruneToSizeNow(0, pruningReason);
    }

    if (maintainMemoryCache == MaintainMemoryCache::No) {
        auto shouldDestroyDecodedDataForAllLiveResources = true;
        MemoryCache::singleton().pruneLiveResourcesToSize(0, shouldDestroyDecodedDataForAllLiveResources);
    }

    CSSValuePool::singleton().drain();
    FontCache::releaseCriticalMemoryInAllFontCaches();
#if ENABLE(WEB_AUDIO)
    HRTFElevation::clearCache();
#endif

    Page::forEachPage([](auto& page) {
        page.cookieJar().clearCache();
    });

    auto allDocuments = Document::allDocuments();
    auto protectedDocuments = WTF::map(allDocuments, [](auto& document) -> Ref<Document> {
        return document.get();
    });
    for (auto& document : protectedDocuments) {
        document->clearQuerySelectorAllResults();
        document->styleScope().releaseMemory();
        if (RefPtr fontSelector = document->fontSelectorIfExists())
            fontSelector->emptyCaches();
        protect(document->cachedResourceLoader())->garbageCollectDocumentResources();

        if (RefPtr pluginDocument = dynamicDowncast<PluginDocument>(document))
            pluginDocument->releaseMemory();

        if (RefPtr localFrame = document->frame())
            protect(localFrame->editor())->releaseMemory();
    }

#if defined(WEBKIT_IOS6)
    if (shouldDeleteAllCodeForMemoryPressure()) {
        // Both numbers, because they are the two scales this decision was accidentally
        // straddling, and a wipe is expensive enough to be worth a line either way.
        WTFLogAlways("[codewipe] footprint %.0f MB, resident %.0f MB, threshold %.0f MB",
            WTF::memoryFootprint() / 1048576.0, residentMegabytes(), codeDeletionThresholdMegabytes());
        if (synchronous == Synchronous::Yes)
            GarbageCollectionController::singleton().deleteAllCode(JSC::PreventCollectionAndDeleteAllCode);
        else
            GarbageCollectionController::singleton().deleteAllCode(JSC::DeleteAllCodeIfNotCollecting);
    }
#else
    if (synchronous == Synchronous::Yes)
        GarbageCollectionController::singleton().deleteAllCode(JSC::PreventCollectionAndDeleteAllCode);
    else
        GarbageCollectionController::singleton().deleteAllCode(JSC::DeleteAllCodeIfNotCollecting);
#endif

#if ENABLE(VIDEO)
    for (auto& mediaElement : HTMLMediaElement::allMediaElements())
        Ref { mediaElement.get() }->purgeBufferedDataIfPossible();
#endif

    if (synchronous == Synchronous::Yes) {
        GarbageCollectionController::singleton().garbageCollectNow();
    } else {
#if PLATFORM(IOS_FAMILY)
        GarbageCollectionController::singleton().garbageCollectNowIfNotDoneRecently();
#else
        GarbageCollectionController::singleton().garbageCollectSoon();
#endif
    }

    WorkerGlobalScope::releaseMemoryInWorkers(synchronous);
}

void releaseMemory(Critical critical, Synchronous synchronous, MaintainBackForwardCache maintainBackForwardCache, MaintainMemoryCache maintainMemoryCache)
{
    TraceScope scope(MemoryPressureHandlerStart, MemoryPressureHandlerEnd, static_cast<uint64_t>(critical), static_cast<uint64_t>(synchronous));

#if PLATFORM(IOS_FAMILY)
    if (critical == Critical::No)
        GarbageCollectionController::singleton().garbageCollectNowIfNotDoneRecently();
#endif

    if (critical == Critical::Yes) {
        // Return unused pages back to the OS now as this will likely give us a little memory to work with.
        WTF::releaseFastMallocFreeMemory();
        releaseCriticalMemory(synchronous, maintainBackForwardCache, maintainMemoryCache);
    }

    releaseNoncriticalMemory(maintainMemoryCache);

    platformReleaseMemory(critical);

    if (synchronous == Synchronous::Yes) {
        // FastMalloc has lock-free thread specific caches that can only be cleared from the thread itself.
        WorkerOrWorkletThread::releaseFastMallocFreeMemoryInAllThreads();
#if ENABLE(SCROLLING_THREAD)
        ScrollingThread::dispatch(WTF::releaseFastMallocFreeMemory);
#endif
        WTF::releaseFastMallocFreeMemory();
    }

#if ENABLE(RESOURCE_USAGE)
    Page::forEachPage([&](Page& page) {
        InspectorInstrumentation::didHandleMemoryPressure(page, critical);
    });
#endif
}

void releaseGraphicsMemory(Critical critical, Synchronous synchronous)
{
    TraceScope scope(MemoryPressureHandlerStart, MemoryPressureHandlerEnd, static_cast<uint64_t>(critical), static_cast<uint64_t>(synchronous));

    platformReleaseGraphicsMemory(critical);

    WTF::releaseFastMallocFreeMemory();
}

#if RELEASE_LOG_DISABLED
void logMemoryStatistics(LogMemoryStatisticsReason) { }
#else
static ASCIILiteral logMemoryStatisticsReasonDescription(LogMemoryStatisticsReason reason)
{
    switch (reason) {
    case LogMemoryStatisticsReason::DebugNotification:
        return "debug notification"_s;
    case LogMemoryStatisticsReason::WarningMemoryPressureNotification:
        return "warning memory pressure notification"_s;
    case LogMemoryStatisticsReason::CriticalMemoryPressureNotification:
        return "critical memory pressure notification"_s;
    case LogMemoryStatisticsReason::OutOfMemoryDeath:
        return "out of memory death"_s;
    };
    RELEASE_ASSERT_NOT_REACHED();
}

void logMemoryStatistics(LogMemoryStatisticsReason reason)
{
    const auto description = logMemoryStatisticsReasonDescription(reason);

    RELEASE_LOG(MemoryPressure, "WebKit memory usage statistics at time of %" PUBLIC_LOG_STRING ":", description.characters());
    RELEASE_LOG(MemoryPressure, "Websam state: %" PUBLIC_LOG_STRING, MemoryPressureHandler::processStateDescription().characters());
    auto stats = PerformanceLogging::memoryUsageStatistics(ShouldIncludeExpensiveComputations::Yes);
    for (auto& [key, val] : stats)
        RELEASE_LOG(MemoryPressure, "%" PUBLIC_LOG_STRING ": %zu", key.characters(), val);

#if PLATFORM(COCOA)
    auto pageSize = vmPageSize();
    auto pages = pagesPerVMTag();

    RELEASE_LOG(MemoryPressure, "Dirty memory per VM tag at time of %" PUBLIC_LOG_STRING ":", description.characters());
    for (unsigned i = 0; i < 256; ++i) {
        size_t dirty = pages[i].dirty * pageSize;
        if (!dirty)
            continue;
        String tagName = displayNameForVMTag(i);
        if (!tagName)
            tagName = makeString("Tag "_s, i);
        RELEASE_LOG(MemoryPressure, "  %" PUBLIC_LOG_STRING ": %lu MB in %zu regions", tagName.latin1().data(), dirty / MB, pages[i].regionCount);
    }

    bool shouldLogJavaScriptObjectCounts = os_variant_allows_internal_security_policies("com.apple.WebKit");
    if (!shouldLogJavaScriptObjectCounts)
        return;
#endif

    auto& vm = commonVM();
    JSC::JSLockHolder locker(vm);
    RELEASE_LOG(MemoryPressure, "Live JavaScript objects at time of %" PUBLIC_LOG_STRING ":", description.characters());
    for (auto& it : vm.heap.objectTypeCounts())
        RELEASE_LOG(MemoryPressure, "  %" PUBLIC_LOG_STRING ": %d", it.key.characters(), it.value);
}
#endif

#if !PLATFORM(COCOA)
#if !USE(SKIA)
void platformReleaseMemory(Critical) { }
#endif
void platformReleaseGraphicsMemory(Critical) { }
void jettisonExpensiveObjectsOnTopLevelNavigation() { }
void registerMemoryReleaseNotifyCallbacks() { }
#endif

} // namespace WebCore
