/*
 * Copyright (C) 2009, 2014 Apple Inc. All rights reserved.
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
#import "LegacyTileCache.h"
#import "MemoryRelease.h"

#if PLATFORM(IOS_FAMILY)

#import "Color.h"
#import "FontAntialiasingStateSaver.h"
#import "LegacyTileGrid.h"
#import "LegacyTileGridTile.h"
#import "LegacyTileLayer.h"
#import "LegacyTileLayerPool.h"
#import "Logging.h"
#import "SystemMemory.h"
#import "WAKWindow.h"
#import "WKGraphics.h"
#import "MemoryCache.h"
#import "WebCoreThreadRun.h"
#import <CoreText/CoreText.h>
#import <pal/spi/cocoa/QuartzCoreSPI.h>
#import <stdlib.h>
#import <wtf/MemoryPressureHandler.h>
#import <wtf/RAMSize.h>

#if defined(WEBKIT_IOS6)
#include <unistd.h>
// Our own running commentary. WTFLogAlways reaches a file through stderr, so
// every one of these is a synchronous write on whichever thread the engine is
// on - and some of them sit on paths that run for every frame of a scroll.
static bool tileCacheChatterEnabled()
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = access("/tmp/native-engine-log", F_OK) == 0 ? 1 : 0;
    return enabled == 1;
}

static unsigned webkitIOS6TileBudgetFromEnvironment(const char* name, unsigned fallbackMegabytes)
{
    const char* value = getenv(name);
    int megabytes = value ? atoi(value) : 0;
    if (megabytes <= 0 || megabytes > 256)
        megabytes = static_cast<int>(fallbackMegabytes);
    return static_cast<unsigned>(megabytes) * 1024 * 1024;
}
#endif


// FIXME: This should go into a WAKViewInternals.h header.
@interface WAKView (WebViewExtras)
- (void)_dispatchTileDidDraw:(CALayer *)tile;
- (void)_willStartScrollingOrZooming;
- (void)_didFinishScrollingOrZooming;
- (void)_scheduleRenderingUpdateForPendingTileCacheRepaint;
@end

@interface LegacyTileCacheTombstone : NSObject {
    BOOL dead;
}
@property(getter=isDead) BOOL dead;
@end

@implementation LegacyTileCacheTombstone
@synthesize dead;
@end

namespace WebCore {

#if defined(WEBKIT_IOS6)
extern "C" int g_webkitIOS6PendingDrawWork;
extern "C" unsigned g_webkitIOS6PaintsRefusedForLayout;
// Raised when a _dispatchTileDidDraw: perform is in flight, lowered by
// -[WebView _dispatchTileDidDraw:] when it runs. See drawLayer().
extern "C" { int g_webkitIOS6TileDidDrawPending = 0; }
#endif

void LegacyTileCache::ref() const
{
    [m_window retain];
}

void LegacyTileCache::deref() const
{
    [m_window release];
}

LegacyTileCache::LegacyTileCache(WAKWindow* window)
    : m_window(window)
    , m_tombstone(adoptNS([[LegacyTileCacheTombstone alloc] init]))
    , m_zoomedOutTileGrid(makeUnique<LegacyTileGrid>(*this, m_tileSize))
    , m_tileCreationTimer(*this, &LegacyTileCache::tileCreationTimerFired)
{
    [hostLayer() insertSublayer:m_zoomedOutTileGrid->tileHostLayer() atIndex:0];
    hostLayerSizeChanged();
}

LegacyTileCache::~LegacyTileCache()
{
    [m_tombstone setDead:true];
}

CGFloat LegacyTileCache::screenScale() const
{
    return [m_window screenScale];
}

CALayer* LegacyTileCache::hostLayer() const
{
    return [m_window hostLayer];
}

FloatRect LegacyTileCache::visibleRectInLayer(CALayer *layer) const
{
    CALayer *host = [m_window hostLayer];
    CGRect rect = m_overrideVisibleRect ? CGRect(m_overrideVisibleRect.value()) : [m_window extendedVisibleRect];
    if (layer == host)
        return rect;
    return [layer convertRect:rect fromLayer:host];
}

bool LegacyTileCache::setOverrideVisibleRect(const FloatRect& rect)
{
    m_overrideVisibleRect = rect;
    auto coveredByExistingTiles = false;
    if (activeTileGrid())
        coveredByExistingTiles = activeTileGrid()->tilesCover(enclosingIntRect(m_overrideVisibleRect.value()));
    return coveredByExistingTiles;
}
    
LegacyTileGrid* LegacyTileCache::activeTileGrid() const
{
    if (!m_keepsZoomedOutTiles)
        return m_zoomedOutTileGrid.get();
    if (m_tilingMode == Zooming)
        return m_zoomedOutTileGrid.get();
    if (m_zoomedInTileGrid && m_currentScale == m_zoomedInTileGrid->scale())
        return m_zoomedInTileGrid.get();
    return m_zoomedOutTileGrid.get();
}

LegacyTileGrid* LegacyTileCache::inactiveTileGrid() const
{
    return activeTileGrid() == m_zoomedOutTileGrid.get() ? m_zoomedInTileGrid.get() : m_zoomedOutTileGrid.get();
}

void LegacyTileCache::setTilesOpaque(bool opaque)
{
    if (m_tilesOpaque == opaque)
        return;

    Locker locker { m_tileMutex };

    m_tilesOpaque = opaque;
    m_zoomedOutTileGrid->updateTileOpacity();
    if (m_zoomedInTileGrid)
        m_zoomedInTileGrid->updateTileOpacity();
}

void LegacyTileCache::doLayoutTiles()
{
    if (isTileCreationSuspended())
        return;

    Locker locker { m_tileMutex };
    LegacyTileGrid* activeGrid = activeTileGrid();
    // Even though we aren't actually creating tiles in the inactive grid, we
    // still need to drop invalid tiles in response to a layout.
    // See <rdar://problem/9839867>.
    if (LegacyTileGrid* inactiveGrid = inactiveTileGrid())
        inactiveGrid->dropInvalidTiles();
    if (activeGrid->checkDoSingleTileLayout())
        return;
    createTilesInActiveGrid(CoverVisibleOnly);
}

void LegacyTileCache::hostLayerSizeChanged()
{
    m_zoomedOutTileGrid->updateHostLayerSize();
    if (m_zoomedInTileGrid)
        m_zoomedInTileGrid->updateHostLayerSize();
}

void LegacyTileCache::setKeepsZoomedOutTiles(bool keep)
{
    m_keepsZoomedOutTiles = keep;
}

void LegacyTileCache::setCurrentScale(float scale)
{
    ASSERT(scale > 0);

    if (currentScale() == scale) {
        m_pendingScale = 0;
        return;
    }
    m_pendingScale = scale;
    if (m_tilingMode == Disabled)
        return;
    commitScaleChange();

    if (!keepsZoomedOutTiles() && !isTileInvalidationSuspended()) {
        // Tile invalidation is normally suspended during zooming by UIKit but some applications
        // using custom scrollviews may zoom without triggering the callbacks. Invalidate the tiles explicitly.
        Locker locker { m_tileMutex };
        activeTileGrid()->dropAllTiles();
        activeTileGrid()->createTiles(CoverVisibleOnly);
    }
}

void LegacyTileCache::setZoomedOutScale(float scale)
{
    ASSERT(scale > 0);

    if (zoomedOutScale() == scale) {
        m_pendingZoomedOutScale = 0;
        return;
    }
    m_pendingZoomedOutScale = scale;
    if (m_tilingMode == Disabled)
        return;
    commitScaleChange();
}
    
void LegacyTileCache::commitScaleChange()
{
    ASSERT(m_pendingZoomedOutScale || m_pendingScale);
    ASSERT(m_tilingMode != Disabled);
    
    Locker locker { m_tileMutex };

    if (m_pendingZoomedOutScale) {
        m_zoomedOutTileGrid->setScale(m_pendingZoomedOutScale);
        m_pendingZoomedOutScale = 0;
    }
    
    if (!m_keepsZoomedOutTiles) {
        ASSERT(activeTileGrid() == m_zoomedOutTileGrid.get());
        if (m_pendingScale) {
            m_currentScale = m_pendingScale;
            m_zoomedOutTileGrid->setScale(m_currentScale);
        }
        m_pendingScale = 0;
        return;
    }

    if (m_pendingScale) {
        m_currentScale = m_pendingScale;
        m_pendingScale = 0;
    }

    if (m_currentScale != m_zoomedOutTileGrid->scale()) {
        if (!m_zoomedInTileGrid) {
            m_zoomedInTileGrid = makeUnique<LegacyTileGrid>(*this, m_tileSize);
            [hostLayer() addSublayer:m_zoomedInTileGrid->tileHostLayer()];
            hostLayerSizeChanged();
        }
        m_zoomedInTileGrid->setScale(m_currentScale);
    }

    // Keep the current ordering during zooming.
    if (m_tilingMode != Zooming)
        bringActiveTileGridToFront();

    adjustTileGridTransforms();
    layoutTiles();
}

void LegacyTileCache::bringActiveTileGridToFront()
{
    LegacyTileGrid* activeGrid = activeTileGrid();
    LegacyTileGrid* otherGrid = inactiveTileGrid();
    if (!otherGrid)
        return;
    CALayer* frontLayer = activeGrid->tileHostLayer();
    CALayer* otherLayer = otherGrid->tileHostLayer();
    [hostLayer() insertSublayer:frontLayer above:otherLayer];
}
    
void LegacyTileCache::adjustTileGridTransforms()
{
    CALayer* zoomedOutHostLayer = m_zoomedOutTileGrid->tileHostLayer();
    float transformScale = currentScale() / zoomedOutScale();
    [zoomedOutHostLayer setTransform:CATransform3DMakeScale(transformScale, transformScale, 1.0f)];
    m_zoomedOutTileGrid->updateHostLayerSize();
}

void LegacyTileCache::layoutTiles()
{
    if (m_hasPendingLayoutTiles)
        return;
    m_hasPendingLayoutTiles = true;

    LegacyTileCacheTombstone *tombstone = m_tombstone.get();
    WebThreadRun(^{
        if ([tombstone isDead])
            return;
        m_hasPendingLayoutTiles = false;
        doLayoutTiles();
    });
}

void LegacyTileCache::layoutTilesNow()
{
    ASSERT(WebThreadIsLockedOrDisabled());

    // layoutTilesNow() is called after a zoom, while the tile mode is still set to Zooming.
    // If we checked for isTileCreationSuspended here, that would cause <rdar://problem/8434112> (Page flashes after zooming in/out).
    if (m_tilingMode == Disabled)
        return;
    
    // FIXME: layoutTilesNow should be called after state has been set to non-zooming and the active grid is the final one. 
    // Fix this in UIKit side (perhaps also getting rid of this call) and remove this code afterwards.
    // <rdar://problem/9672993>
    TilingMode savedTilingMode = m_tilingMode;
    if (m_tilingMode == Zooming)
        m_tilingMode = Minimal;

    Locker locker { m_tileMutex };
    LegacyTileGrid* activeGrid = activeTileGrid();
    if (activeGrid->checkDoSingleTileLayout()) {
        m_tilingMode = savedTilingMode;
        return;
    }
    createTilesInActiveGrid(CoverVisibleOnly);
    m_tilingMode = savedTilingMode;
}

void LegacyTileCache::layoutTilesNowForRect(const IntRect& rect)
{
    ASSERT(WebThreadIsLockedOrDisabled());
    Locker locker { m_tileMutex };

    activeTileGrid()->addTilesCoveringRect(rect);
}

void LegacyTileCache::removeAllNonVisibleTiles()
{
    Locker locker { m_tileMutex };
    removeAllNonVisibleTilesInternal();
}

void LegacyTileCache::removeAllNonVisibleTilesInternal()
{
    LegacyTileGrid* activeGrid = activeTileGrid();
    if (keepsZoomedOutTiles() && activeGrid == m_zoomedInTileGrid.get() && activeGrid->hasTiles())
        m_zoomedOutTileGrid->dropAllTiles();

    IntRect activeTileBounds = activeGrid->bounds();
    if (activeTileBounds.width() <= m_tileSize.width() && activeTileBounds.height() <= m_tileSize.height()) {
        // If the view is smaller than a tile, keep the tile even if it is not visible.
        activeGrid->dropTilesOutsideRect(activeTileBounds);
        return;
    }

    activeGrid->dropTilesOutsideRect(activeGrid->visibleRect());
}

void LegacyTileCache::removeAllTiles()
{
    Locker locker { m_tileMutex };
    m_zoomedOutTileGrid->dropAllTiles();
    if (m_zoomedInTileGrid)
        m_zoomedInTileGrid->dropAllTiles();
}

void LegacyTileCache::removeForegroundTiles()
{
    Locker locker { m_tileMutex };
    if (!keepsZoomedOutTiles())
        m_zoomedOutTileGrid->dropAllTiles();
    if (m_zoomedInTileGrid)
        m_zoomedInTileGrid->dropAllTiles();
}

void LegacyTileCache::setContentReplacementImage(RetainPtr<CGImageRef> contentReplacementImage)
{
    Locker locker { m_contentReplacementImageMutex };
    m_contentReplacementImage = contentReplacementImage;
}

RetainPtr<CGImageRef> LegacyTileCache::contentReplacementImage() const
{
    Locker locker { m_contentReplacementImageMutex };
    return m_contentReplacementImage;
}

void LegacyTileCache::setTileBordersVisible(bool flag)
{
    if (flag == m_tileBordersVisible)
        return;

    m_tileBordersVisible = flag;
    m_zoomedOutTileGrid->updateTileBorderVisibility();
    if (m_zoomedInTileGrid)
        m_zoomedInTileGrid->updateTileBorderVisibility();
}

void LegacyTileCache::setTilePaintCountersVisible(bool flag)
{
    m_tilePaintCountersVisible = flag;
    // The numbers will show up the next time the tiles paint.
}

void LegacyTileCache::finishedCreatingTiles(bool didCreateTiles, bool createMore)
{
    // We need to ensure that all tiles are showing the same version of the content.
    if (didCreateTiles && !m_savedDisplayRects.isEmpty())
        flushSavedDisplayRects();

    if (keepsZoomedOutTiles()) {
        if (m_zoomedInTileGrid && activeTileGrid() == m_zoomedOutTileGrid.get() && m_tilingMode != Zooming && m_zoomedInTileGrid->hasTiles()) {
            // This CA transaction will cover the screen with top level tiles.
            // We can remove zoomed-in tiles without flashing.
            m_zoomedInTileGrid->dropAllTiles();
        } else if (activeTileGrid() == m_zoomedInTileGrid.get()) {
            // Pass the minimum possible distance to consider all tiles, even visible ones.
            m_zoomedOutTileGrid->dropDistantTiles(0, std::numeric_limits<double>::min(), m_zoomedOutTileGrid->visibleRect());
        }
    }

    // Keep creating tiles until the whole coverRect is covered.
    if (createMore)
        m_tileCreationTimer.startOneShot(0_s);
}

void LegacyTileCache::tileCreationTimerFired()
{
    if (isTileCreationSuspended())
        return;
    Locker locker { m_tileMutex };
    createTilesInActiveGrid(CoverSpeculative);
}

void LegacyTileCache::createTilesInActiveGrid(SynchronousTileCreationMode mode)
{
#if defined(WEBKIT_IOS6)
    if (MemoryPressureHandler::singleton().memoryPressureStatus() == SystemMemoryPressureStatus::Critical) {
#else
    if (MemoryPressureHandler::singleton().isUnderMemoryPressure()) {
#endif
        LOG(MemoryPressure, "Under memory pressure at: %s", __PRETTY_FUNCTION__);
        removeAllNonVisibleTilesInternal();
    }
    activeTileGrid()->createTiles(mode);
}

unsigned LegacyTileCache::tileCapacityForGrid(LegacyTileGrid* grid)
{
    static unsigned capacity;
    if (!capacity) {
#if defined(WEBKIT_IOS6)
        capacity = webkitIOS6TileBudgetFromEnvironment("WEBKIT_IOS6_TILE_BUDGET_MB", 24);
#else
        size_t totalMemory = ramSize() / 1024 / 1024;
        if (totalMemory >= 1024)
            capacity = 128 * 1024 * 1024;
        else if (totalMemory >= 512)
            capacity = 64 * 1024 * 1024;
        else
            capacity = 32 * 1024 * 1024;
#endif
    }

    int gridCapacity;

    int memoryLevel = systemMemoryLevel();
    if (memoryLevel < 15)
        gridCapacity = capacity / 4;
    else if (memoryLevel < 20)
        gridCapacity = capacity / 2;
    else if (memoryLevel < 30) 
        gridCapacity = capacity * 3 / 4;
    else
        gridCapacity = capacity;

#if defined(WEBKIT_IOS6)
    // The active grid has to hold the whole cover rect with room to spare, or
    // dropDistantTiles() refuses and createTiles() returns having created
    // nothing. A 1380 px cover rect against a 12 MB grid painted 18, 18 and 17%
    // of the feed through ten 400 px flicks - the worst of every pairing tried.
    // The same coverage against a 36 MB grid painted 44%, and the 2300 px rect
    // this port now asks for against this 18 MB grid painted 31 to 62%.
    //
    // The level read above is kern.memorystatus_level, which is the whole
    // system's free memory, and on this device the page is most of it - so the
    // tiering cuts the grid to three tiles exactly while the reader is
    // scrolling, which is the one moment it is needed. A floor rather than a
    // rewrite: the ceiling still tiers, and real pressure is still answered by
    // removeAllNonVisibleTilesInternal() and releaseMemory() above.
    //
    // On a 24000 px static page, where layout is cheap, three tiles and eleven
    // paint the same: every frame of ten flicks whole, at 76 and 94 MB
    // resident. Nothing here is what leaves the feed unpainted.
    static const unsigned activeGridFloor = webkitIOS6TileBudgetFromEnvironment("WEBKIT_IOS6_TILE_FLOOR_MB", 18);
    if (gridCapacity < static_cast<int>(activeGridFloor * 4 / 3))
        gridCapacity = static_cast<int>(activeGridFloor * 4 / 3);
#endif

    static int lastReportedLevel = -1;
    if (memoryLevel != lastReportedLevel) {
        lastReportedLevel = memoryLevel;
        if (tileCacheChatterEnabled()) WTFLogAlways("[tilebudget] systemMemoryLevel %d -> grid capacity %.1f MB (active grid gets %.1f MB)",
            memoryLevel, gridCapacity / (1024.0 * 1024.0), (gridCapacity * 3 / 4) / (1024.0 * 1024.0));
    }

    // This poll is the only place the engine notices the system running out of
    // memory before the kill arrives - both soak deaths were straight SIGKILLs
    // with the level in the twenties just before. Shrinking the tile grid alone
    // saves a few megabytes; the caches and the collector hold far more, and
    // this tells them too. Rate-limited so a level hovering at the threshold
    // does not turn into a purge loop.
    // The critical level of this call throws away every compiled script, the
    // font caches and the style resolver. Measured on the feed, doing that on a
    // ten second timer left the engine permanently regenerating bytecode under
    // the web lock and the interface at one frame per second. So the routine
    // answer to pressure is a collection and the cheap caches; the destructive
    // one is kept for the last few megabytes before the kill.
    if (memoryLevel > 0 && memoryLevel < 25) {
        static CFAbsoluteTime lastRelease;
        static CFAbsoluteTime lastCriticalRelease;
        CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
        bool desperate = memoryLevel < 10;
        double sinceLast = now - (desperate ? lastCriticalRelease : lastRelease);
        if (sinceLast > (desperate ? 60.0 : 20.0)) {
            if (desperate)
                lastCriticalRelease = now;
            lastRelease = now;
            if (tileCacheChatterEnabled()) WTFLogAlways("[tilebudget] level %d: releasing engine memory%s", memoryLevel, desperate ? " (critical)" : "");
            WebThreadRun(^{
                // The largest identified block of this process's memory is
                // decoded image data: the layer count is five to eleven and the
                // tile cache under five megabytes, while the graphics figure sits
                // near sixty. Those images are live - they are in the render tree
                // - so an ordinary cache prune leaves them alone. Dropping the
                // decoded form of the ones furthest from the screen costs a
                // redecode if the reader scrolls back, and costs nothing else.
                MemoryCache::singleton().pruneLiveResourcesToSize(4 * 1024 * 1024, false);
                WebCore::releaseMemory(desperate ? WTF::Critical::Yes : WTF::Critical::No, WTF::Synchronous::No);
            });
        }
    }

    if (keepsZoomedOutTiles() && grid == m_zoomedOutTileGrid.get()) {
        if (activeTileGrid() == m_zoomedOutTileGrid.get())
            return gridCapacity;
        return gridCapacity / 4;
    }
    return gridCapacity * 3 / 4;
}

Color LegacyTileCache::colorForGridTileBorder(LegacyTileGrid* grid) const
{
    if (grid == m_zoomedOutTileGrid.get())
        return SRGBA<uint8_t> { 51, 255, 0, 128 };
    return SRGBA<uint8_t> { 51, 230, 0, 128 };
}

static bool shouldRepaintInPieces(const CGRect& dirtyRect, CGSRegionObj dirtyRegion, CGFloat contentsScale)
{
    // Estimate whether or not we should use the unioned rect or the individual rects.
    // We do this by computing the percentage of "wasted space" in the union. If that wasted space
    // is too large, then we will do individual rect painting instead.
    float singlePixels = 0;
    unsigned rectCount = 0;

    CGSRegionEnumeratorObj enumerator = CGSRegionEnumerator(dirtyRegion);
    CGRect *subRect;
    while ((subRect = CGSNextRect(enumerator))) {
        ++rectCount;
        singlePixels += subRect->size.width * subRect->size.height;
    }
    singlePixels /= (contentsScale * contentsScale);
    CGSReleaseRegionEnumerator(enumerator);

    const unsigned cRectThreshold = 10;
    if (rectCount < 2 || rectCount > cRectThreshold)
        return false;

    const float cWastedSpaceThreshold = 0.50f;
    float unionPixels = dirtyRect.size.width * dirtyRect.size.height;
    float wastedSpace = 1.f - (singlePixels / unionPixels);
    return wastedSpace > cWastedSpaceThreshold;
}

void LegacyTileCache::drawReplacementImage(LegacyTileLayer* layer, CGContextRef context, CGImageRef image)
{
    CGContextSetRGBFillColor(context, 1, 1, 1, 1);
    CGContextFillRect(context, CGContextGetClipBoundingBox(context));

    CGFloat contentsScale = [layer contentsScale];
    CGContextScaleCTM(context, 1 / contentsScale, -1 / contentsScale);
    CGRect imageRect = CGRectMake(0, 0, CGImageGetWidth(image), CGImageGetHeight(image));
    CGContextTranslateCTM(context, 0, -imageRect.size.height);
    CGContextDrawImage(context, imageRect, image);
}

void LegacyTileCache::drawWindowContent(LegacyTileLayer* layer, CGContextRef context, CGRect dirtyRect, DrawingFlags drawingFlags, CGRect frame)
{
    // +[WAKWindow hasLandscapeOrientation] calls out to the application's
    // orientation provider; the saver only looks at it when the style is being
    // overridden, so it is not asked for otherwise.
    bool useOrientationDependentFontAntialiasing = [m_window useOrientationDependentFontAntialiasing] && [layer isOpaque];
    FontAntialiasingStateSaver fontAntialiasingState(context, useOrientationDependentFontAntialiasing);
    fontAntialiasingState.setup(useOrientationDependentFontAntialiasing && [WAKWindow hasLandscapeOrientation]);

    if (drawingFlags == DrawingFlags::Snapshotting)
        [m_window setIsInSnapshottingPaint:YES];
        
    CGSRegionObj drawRegion = (CGSRegionObj)[layer regionBeingDrawn];
    CGFloat contentsScale = [layer contentsScale];

    // hostLayer() is -[WAKWindow hostLayer]; both branches below need it, and
    // the simple one used to ask for it again after drawLayer() already had.
    CALayer *host = hostLayer();

    if (drawRegion && shouldRepaintInPieces(dirtyRect, drawRegion, contentsScale)) {
        // Use fine grained repaint rectangles to minimize the amount of painted pixels.
        CGSRegionEnumeratorObj enumerator = CGSRegionEnumerator(drawRegion);
        CGRect *subRect;
        while ((subRect = CGSNextRect(enumerator))) {
            CGRect adjustedSubRect = *subRect;
            adjustedSubRect.origin.x /= contentsScale;
            adjustedSubRect.origin.y = frame.size.height - (adjustedSubRect.origin.y + adjustedSubRect.size.height) / contentsScale;
            adjustedSubRect.size.width /= contentsScale;
            adjustedSubRect.size.height /= contentsScale;

            CGRect subRectInSuper = [host convertRect:adjustedSubRect fromLayer:layer];
            [m_window displayRect:subRectInSuper];
        }
        CGSReleaseRegionEnumerator(enumerator);
    } else {
        // Simple repaint
        CGRect dirtyRectInSuper = [host convertRect:dirtyRect fromLayer:layer];
#if defined(WEBKIT_IOS6)
        if (([]() { static const bool logTilePaintOnce = getenv("WEBKIT_IOS6_LOG_PAINT") != nullptr; return logTilePaintOnce; }())) {
            fprintf(stderr, "[ios6 paint] tile %g,%g %gx%g dirty %g,%g %gx%g content view %s\n",
                frame.origin.x, frame.origin.y, frame.size.width, frame.size.height,
                dirtyRectInSuper.origin.x, dirtyRectInSuper.origin.y,
                dirtyRectInSuper.size.width, dirtyRectInSuper.size.height,
                [m_window contentView] ? object_getClassName([m_window contentView]) : "(none)");
            fflush(stderr);
        }
#endif
        [m_window displayRect:dirtyRectInSuper];
    }
    
    if (drawingFlags == DrawingFlags::Snapshotting)
        [m_window setIsInSnapshottingPaint:NO];
}

void LegacyTileCache::drawLayer(LegacyTileLayer* layer, CGContextRef context, DrawingFlags drawingFlags)
{
    // The web lock unlock observer runs after CA commit observer.
    if (!WebThreadIsLockedOrDisabled()) {
        LOG_ERROR("Drawing without holding the web thread lock");
        ASSERT_NOT_REACHED();
    }

    WKSetCurrentGraphicsContext(context);


    CGRect dirtyRect = CGContextGetClipBoundingBox(context);
    CGRect frame = [layer frame];
    CGContextTranslateCTM(context, -frame.origin.x, -frame.origin.y);
    CGRect scaledFrame = [hostLayer() convertRect:[layer bounds] fromLayer:layer];
    CGContextScaleCTM(context, frame.size.width / scaledFrame.size.width, frame.size.height / scaledFrame.size.height);

#if defined(WEBKIT_IOS6)
    unsigned refusedPaintsBeforeDrawing = g_webkitIOS6PaintsRefusedForLayout;
#endif

    if (RetainPtr<CGImage> contentReplacementImage = this->contentReplacementImage())
        drawReplacementImage(layer, context, contentReplacementImage.get());
    else
        drawWindowContent(layer, context, dirtyRect, drawingFlags, frame);

#if defined(WEBKIT_IOS6)
    if (drawingFlags != DrawingFlags::Snapshotting && g_webkitIOS6PaintsRefusedForLayout != refusedPaintsBeforeDrawing)
        setNeedsDisplayInRect(enclosingIntRect(FloatRect(frame)));
#endif

    ++layer.paintCount;
    if (m_tilePaintCountersVisible) {
        auto string = adoptCF(CFStringCreateWithFormat(0, 0, CFSTR("%d"), layer.paintCount));

        CGContextSaveGState(context);

        CGContextTranslateCTM(context, frame.origin.x, frame.origin.y);
        CGContextSetFillColorWithColor(context, cachedCGColor(colorForGridTileBorder([layer tileGrid])).get());
        
        CGRect labelBounds = [layer bounds];
        labelBounds.size.width = 10 + 12 * CFStringGetLength(string.get());
        labelBounds.size.height = 25;
        CGContextFillRect(context, labelBounds);

        if (acceleratedDrawingEnabled())
            CGContextSetRGBFillColor(context, 1, 0, 0, 0.4f);
        else
            CGContextSetRGBFillColor(context, 1, 1, 1, 0.6f);

        auto matrix = CGAffineTransformMakeScale(1, -1);
        auto font = adoptCF(CTFontCreateWithName(CFSTR("Helvetica"), 25, &matrix));
        CFTypeRef keys[] = { kCTFontAttributeName, kCTForegroundColorFromContextAttributeName };
        CFTypeRef values[] = { font.get(), kCFBooleanTrue };
        auto attributes = adoptCF(CFDictionaryCreate(kCFAllocatorDefault, keys, values, std::size(keys), &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        auto attributedString = adoptCF(CFAttributedStringCreate(kCFAllocatorDefault, string.get(), attributes.get()));
        auto line = adoptCF(CTLineCreateWithAttributedString(attributedString.get()));
        CGContextSetTextPosition(context, labelBounds.origin.x + 3, labelBounds.origin.y + 20);
        CTLineDraw(line.get(), context);
    
        CGContextRestoreGState(context);        
    }

    WAKView* view = [m_window contentView];
#if defined(WEBKIT_IOS6)
    // One outstanding "tiles drew" notification, not one per tile.
    //
    // -[WebView _dispatchTileDidDraw:] reports the first paint of a load and
    // returns immediately on every call after it, but the delayed perform that
    // carries it allocates a timer and wakes the run loop each time. On the web
    // thread that wake is a whole extra turn of the loop, which means another
    // acquire and release of the web lock plus an autorelease pool - paid once
    // per tile, per drawing pass. The flag is lowered by the callback itself, so
    // no notification is dropped: a pass that finds one already in flight is a
    // pass whose notification has not been delivered yet.
    if (view && !g_webkitIOS6TileDidDrawPending) {
        g_webkitIOS6TileDidDrawPending = 1;
        [view performSelector:@selector(_dispatchTileDidDraw:) withObject:layer afterDelay:0.0];
    }
#else
    [view performSelector:@selector(_dispatchTileDidDraw:) withObject:layer afterDelay:0.0];
#endif
}

void LegacyTileCache::setNeedsDisplay()
{
    setNeedsDisplayInRect(IntRect(0, 0, std::numeric_limits<int>::max(), std::numeric_limits<int>::max()));
}

void LegacyTileCache::scheduleRenderingUpdateForPendingRepaint()
{
    WAKView* view = [m_window contentView];
    [view _scheduleRenderingUpdateForPendingTileCacheRepaint];
}

void LegacyTileCache::setNeedsDisplayInRect(const IntRect& dirtyRect)
{
    g_webkitIOS6PendingDrawWork = 1;
    Locker locker { m_savedDisplayRectMutex };
    bool addedFirstRect = m_savedDisplayRects.isEmpty();
#if defined(WEBKIT_IOS6)
    // A repaint contained in one already queued invalidates exactly the same
    // pixels of exactly the same tiles, so queueing it only makes
    // flushSavedDisplayRects() walk the grid again for nothing. Pages that
    // invalidate an element and then its parent do this constantly.
    //
    // The parent-then-element order is just as common, so the swallowing goes
    // both ways: a rect that covers the one already queued replaces it instead
    // of adding a second walk of the grid over the same pixels.
    if (!addedFirstRect) {
        IntRect& lastRect = m_savedDisplayRects.last();
        if (lastRect.contains(dirtyRect))
            return;
        if (dirtyRect.contains(lastRect)) {
            lastRect = dirtyRect;
            return;
        }
    }
#endif
    m_savedDisplayRects.append(dirtyRect);
    if (!addedFirstRect)
        return;
    // Compositing layer flush will call back to doPendingRepaints(). The flush may be throttled and not happen immediately.
    scheduleRenderingUpdateForPendingRepaint();
}

void LegacyTileCache::invalidateTiles(const IntRect& dirtyRect)
{
    ASSERT(!m_tileMutex.tryLock());

    LegacyTileGrid* activeGrid = activeTileGrid();
    if (!keepsZoomedOutTiles()) {
        activeGrid->invalidateTiles(dirtyRect);
        return;
    }
    FloatRect scaledRect = dirtyRect;
    scaledRect.scale(zoomedOutScale() / currentScale());
    IntRect zoomedOutDirtyRect = enclosingIntRect(scaledRect);
    if (activeGrid == m_zoomedOutTileGrid.get()) {
        bool dummy;
        IntRect coverRect = m_zoomedOutTileGrid->calculateCoverRect(m_zoomedOutTileGrid->visibleRect(), dummy);
        // Instead of repainting a tile outside the cover rect, just remove it.
        m_zoomedOutTileGrid->dropTilesBetweenRects(zoomedOutDirtyRect, coverRect);
        m_zoomedOutTileGrid->invalidateTiles(zoomedOutDirtyRect);
        // We need to invalidate zoomed in tiles as well while zooming, since
        // we could switch back to the zoomed in grid without dropping its
        // tiles.  See <rdar://problem/9946759>.
        if (m_tilingMode == Zooming && m_zoomedInTileGrid)
            m_zoomedInTileGrid->invalidateTiles(dirtyRect);
        return;
    }
    if (!m_zoomedInTileGrid->hasTiles()) {
        // If no tiles have been created yet for the zoomed in grid, we can't drop the zoomed out tiles.
        m_zoomedOutTileGrid->invalidateTiles(zoomedOutDirtyRect);
        return;
    }
    m_zoomedOutTileGrid->dropTilesIntersectingRect(zoomedOutDirtyRect);
    m_zoomedInTileGrid->invalidateTiles(dirtyRect);
}
    
bool LegacyTileCache::isTileCreationSuspended() const 
{
    return (!keepsZoomedOutTiles() && m_tilingMode == Zooming) || m_tilingMode == Disabled;
}

bool LegacyTileCache::isTileInvalidationSuspended() const 
{ 
    if (m_tilingMode == Panning) {
        LegacyTileGrid* grid = const_cast<LegacyTileCache*>(this)->activeTileGrid();
        if (grid && grid->shouldUseMinimalTileCoverage())
            return false;
    }
    return m_tilingMode == Zooming || m_tilingMode == Panning || m_tilingMode == ScrollToTop || m_tilingMode == Disabled; 
}

void LegacyTileCache::updateTilingMode()
{
    ASSERT(WebThreadIsCurrent() || !WebThreadIsEnabled());

    WAKView* view = [m_window contentView];

    if (m_tilingMode == Zooming || m_tilingMode == Panning || m_tilingMode == ScrollToTop) {
        if (!m_didCallWillStartScrollingOrZooming) {
            [view _willStartScrollingOrZooming];
            m_didCallWillStartScrollingOrZooming = true;
        }
    } else {
        if (m_didCallWillStartScrollingOrZooming) {
            [view _didFinishScrollingOrZooming];
            m_didCallWillStartScrollingOrZooming = false;
        }
        if (m_tilingMode == Disabled)
            return;

        Locker locker { m_tileMutex };
        createTilesInActiveGrid(CoverVisibleOnly);

        if (!m_savedDisplayRects.isEmpty())
            scheduleRenderingUpdateForPendingRepaint();
    }
}

void LegacyTileCache::setTilingMode(TilingMode tilingMode)
{
    if (tilingMode == m_tilingMode)
        return;
    bool wasZooming = (m_tilingMode == Zooming);
    m_tilingMode = tilingMode;

    if ((m_pendingZoomedOutScale || m_pendingScale) && m_tilingMode != Disabled)
        commitScaleChange();
    else if (wasZooming) {
        Locker locker { m_tileMutex };
        bringActiveTileGridToFront();
    }

    if (m_hasPendingUpdateTilingMode)
        return;
    m_hasPendingUpdateTilingMode = true;

    LegacyTileCacheTombstone *tombstone = m_tombstone.get();
    WebThreadRun(^{
        if ([tombstone isDead])
            return;
        m_hasPendingUpdateTilingMode = false;
        updateTilingMode();
    });
}
    
float LegacyTileCache::zoomedOutScale() const
{
    return m_zoomedOutTileGrid->scale();
}

float LegacyTileCache::currentScale() const
{
    return m_currentScale;
}

void LegacyTileCache::doPendingRepaints()
{
    if (m_savedDisplayRects.isEmpty())
        return;
    if (isTileInvalidationSuspended())
        return;
    Locker locker { m_tileMutex };
    flushSavedDisplayRects();
}

void LegacyTileCache::flushSavedDisplayRects()
{
    ASSERT(!m_tileMutex.tryLock());
    ASSERT(!m_savedDisplayRects.isEmpty());

    Vector<IntRect> rects;
    {
        Locker locker { m_savedDisplayRectMutex };
        m_savedDisplayRects.swap(rects);
    }
    size_t size = rects.size();
    for (size_t n = 0; n < size; ++n)
        invalidateTiles(rects[n]);
}

void LegacyTileCache::setSpeculativeTileCreationEnabled(bool enabled)
{
    if (m_isSpeculativeTileCreationEnabled == enabled)
        return;
    m_isSpeculativeTileCreationEnabled = enabled;
    if (m_isSpeculativeTileCreationEnabled)
        m_tileCreationTimer.startOneShot(0_s);
}

// Whether the engine has anything for prepareToDraw to do.
//
// -[LegacyTileLayer layoutSublayers] runs inside every CoreAnimation layout pass
// on the main thread and took the web lock unconditionally, so the interface
// waited for whatever the engine happened to be doing - measured on the device
// as three waits longer than a second in a single scroll, the worst of them
// 16.4 seconds, while 97% of acquisitions were under a frame. The median was
// never the problem; the tail was.
//
// The engine raises this when it schedules a layout or invalidates a tile
// rectangle, and prepareToDraw lowers it. When it is down there is nothing to
// ask the engine for, so the main thread does not ask, and cannot be made to
// wait. A plain relaxed atomic: a stale read costs one frame in either
// direction, and the safety valve below bounds that anyway.
extern "C" { int g_webkitIOS6PendingDrawWork = 1; }
extern "C" { unsigned g_webkitIOS6PaintsRefusedForLayout = 0; }

static double webkitIOS6EngineIntervalFromEnvironment(const char* name, double fallback)
{
    const char* value = getenv(name);
    if (!value)
        return fallback;
    double milliseconds = atof(value);
    if (milliseconds < 0)
        return fallback;
    return milliseconds / 1000.0;
}

// When the main thread has to stop asking politely and simply wait.
//
// Skipping prepareToDraw skips the compositing flush with it, and that flush is
// what puts new content into composited layers. Tiles keep painting themselves,
// so the page looked right while every fixed bar on it stayed empty - the engine
// reported the bars at their correct places, y 20 and y 430 in window
// coordinates, and the screen showed neither. Content without its bars is not a
// win.
//
// So the skipping is bounded: after this long without a real pass, the main
// thread waits however long one engine operation takes. That is a bounded stall
// - the layouts are around 300 ms now - in exchange for the page being whole.
bool LegacyTileCache::mainThreadMustWaitForEngine()
{
    static const double mustWaitAfter = webkitIOS6EngineIntervalFromEnvironment("WEBKIT_IOS6_ENGINE_MUST_WAIT_MS", 0.15);
    return CFAbsoluteTimeGetCurrent() - lastPreparedToDraw() > mustWaitAfter;
}

double& LegacyTileCache::lastPreparedToDraw()
{
    static double when;
    return when;
}

bool LegacyTileCache::mainThreadShouldWaitForEngine()
{
    if (g_webkitIOS6PendingDrawWork)
        return true;

    // Never skipped for long. If the engine has been quiet but something was
    // missed - a case that raises no flag - this puts the main thread back in
    // step at four times a second, which is invisible but self-correcting.
    static const double resyncAfter = webkitIOS6EngineIntervalFromEnvironment("WEBKIT_IOS6_ENGINE_RESYNC_MS", 0.25);
    static CFAbsoluteTime lastSynchronised;
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    if (now - lastSynchronised > resyncAfter) {
        lastSynchronised = now;
        return true;
    }
    return false;
}

void LegacyTileCache::prepareToDraw()
{
#if defined(WEBKIT_IOS6)
    g_webkitIOS6PendingDrawWork = 0;
#endif

    // This will trigger document relayout if needed.
    [[m_window contentView] viewWillDraw];

    if (!m_savedDisplayRects.isEmpty()) {
        Locker locker { m_tileMutex };
        flushSavedDisplayRects();
    }

#if !defined(WEBKIT_IOS6)
    g_webkitIOS6PendingDrawWork = 0;
#endif
    lastPreparedToDraw() = CFAbsoluteTimeGetCurrent();
}

void LegacyTileCache::setLayerPoolCapacity(unsigned capacity)
{
    LegacyTileLayerPool::sharedPool()->setCapacity(capacity);
}

void LegacyTileCache::drainLayerPool()
{
    LegacyTileLayerPool::sharedPool()->drain();
}

void LegacyTileCache::dumpTiles()
{
    NSLog(@"=================");
    NSLog(@"ZOOMED OUT");
    if (m_zoomedOutTileGrid.get() == activeTileGrid())
        NSLog(@"<ACTIVE>");
    m_zoomedOutTileGrid->dumpTiles();
    NSLog(@"=================");
    if (m_zoomedInTileGrid) {
        NSLog(@"ZOOMED IN");
        if (m_zoomedInTileGrid.get() == activeTileGrid())
            NSLog(@"<ACTIVE>");
        m_zoomedInTileGrid->dumpTiles();
        NSLog(@"=================");
    }
}

} // namespace WebCore

#endif // PLATFORM(IOS_FAMILY)
