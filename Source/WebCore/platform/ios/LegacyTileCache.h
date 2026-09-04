/*
 * Copyright (C) 2009-2021 Apple Inc. All rights reserved.
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

#pragma once

#include <wtf/Platform.h>
#if PLATFORM(IOS_FAMILY)

#include <WebCore/FloatRect.h>
#include <WebCore/IntRect.h>
#include <WebCore/Timer.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/RetainPtr.h>
#include <wtf/Vector.h>
#include <wtf/WeakPtr.h>

OBJC_CLASS CALayer;
OBJC_CLASS LegacyTileCacheTombstone;
OBJC_CLASS LegacyTileLayer;
OBJC_CLASS WAKWindow;

namespace WebCore {

class Color;
class LegacyTileGrid;

class LegacyTileCache : public CanMakeWeakPtr<LegacyTileCache> {
    WTF_MAKE_NONCOPYABLE(LegacyTileCache);
public:
    LegacyTileCache(WAKWindow *);
    ~LegacyTileCache();

    void ref() const;
    void deref() const;

    CGFloat screenScale() const;

    void setNeedsDisplay();
    void setNeedsDisplayInRect(const IntRect&);
    
    void layoutTiles();
    void layoutTilesNow();
    void layoutTilesNowForRect(const IntRect&);
    void removeAllNonVisibleTiles();
    void removeAllTiles();
    void removeForegroundTiles();

    // If 'contentReplacementImage' is not NULL, drawLayer() draws
    // contentReplacementImage instead of the page content. We assume the
    // image is to be drawn at the origin and scaled to match device pixels.
    void setContentReplacementImage(RetainPtr<CGImageRef>);
    RetainPtr<CGImageRef> contentReplacementImage() const;

    WEBCORE_EXPORT void setTileBordersVisible(bool);
    bool tileBordersVisible() const { return m_tileBordersVisible; }

    WEBCORE_EXPORT void setTilePaintCountersVisible(bool);
    bool tilePaintCountersVisible() const { return m_tilePaintCountersVisible; }

    void setAcceleratedDrawingEnabled(bool enabled) { m_acceleratedDrawingEnabled = enabled; }
    bool acceleratedDrawingEnabled() const { return m_acceleratedDrawingEnabled; }

    void setKeepsZoomedOutTiles(bool);
    bool keepsZoomedOutTiles() const { return m_keepsZoomedOutTiles; }

    void setZoomedOutScale(float);
    float zoomedOutScale() const;
    
    void setCurrentScale(float);
    float currentScale() const;
    
    bool tilesOpaque() const { return m_tilesOpaque; }
    void setTilesOpaque(bool);
    
    enum TilingMode {
        Normal,
        Minimal,
        Panning,
        Zooming,
        Disabled,
        ScrollToTop
    };
    TilingMode tilingMode() const { return m_tilingMode; }
    void setTilingMode(TilingMode);

    enum TilingDirection {
        TilingDirectionUp,
        TilingDirectionDown,
        TilingDirectionLeft,
        TilingDirectionRight,
    };
    void setTilingDirection(TilingDirection tilingDirection) { m_tilingDirection = tilingDirection; }
    TilingDirection tilingDirection() const { return m_tilingDirection; }

    void hostLayerSizeChanged();

    WEBCORE_EXPORT static void setLayerPoolCapacity(unsigned);
    WEBCORE_EXPORT static void drainLayerPool();

    // Logging
    void dumpTiles();

    // Internal
    void doLayoutTiles();
    
    enum class DrawingFlags { None, Snapshotting };
    void drawLayer(LegacyTileLayer *, CGContextRef, DrawingFlags);
    void prepareToDraw();
#if defined(WEBKIT_IOS6)
    static bool mainThreadShouldWaitForEngine();
    WEBCORE_EXPORT static bool mainThreadMustWaitForEngine();
    static double& lastPreparedToDraw();
#endif
    void finishedCreatingTiles(bool didCreateTiles, bool createMore);
    FloatRect visibleRectInLayer(CALayer *) const;
    CALayer* hostLayer() const;
    unsigned tileCapacityForGrid(LegacyTileGrid*);
    Color colorForGridTileBorder(LegacyTileGrid*) const;
    bool setOverrideVisibleRect(const FloatRect&);
    void clearOverrideVisibleRect() { m_overrideVisibleRect = std::nullopt; }

    void doPendingRepaints();

    bool isSpeculativeTileCreationEnabled() const { return m_isSpeculativeTileCreationEnabled; }
    void setSpeculativeTileCreationEnabled(bool);
    
    enum SynchronousTileCreationMode { CoverVisibleOnly, CoverSpeculative };

    bool tileControllerShouldUseLowScaleTiles() const { return m_tileControllerShouldUseLowScaleTiles; } 
    void setTileControllerShouldUseLowScaleTiles(bool flag) { m_tileControllerShouldUseLowScaleTiles = flag; } 

private:
    LegacyTileGrid* activeTileGrid() const;
    LegacyTileGrid* inactiveTileGrid() const;

    void updateTilingMode();
    bool isTileInvalidationSuspended() const;
    bool isTileCreationSuspended() const;
    void flushSavedDisplayRects();
    void invalidateTiles(const IntRect& dirtyRect);
    void setZoomedOutScaleInternal(float);
    void commitScaleChange();
    void bringActiveTileGridToFront();
    void adjustTileGridTransforms();
    void removeAllNonVisibleTilesInternal();
    void createTilesInActiveGrid(SynchronousTileCreationMode);
    void scheduleRenderingUpdateForPendingRepaint();

    void tileCreationTimerFired();

    void drawReplacementImage(LegacyTileLayer *, CGContextRef, CGImageRef);
    void drawWindowContent(LegacyTileLayer *, CGContextRef, CGRect dirtyRect, DrawingFlags, CGRect layerFrame);

    WAKWindow *m_window { nullptr };

    RetainPtr<CGImageRef> m_contentReplacementImage;

    // Ensure there are no async calls on a dead tile cache.
    RetainPtr<LegacyTileCacheTombstone> m_tombstone;

    std::optional<FloatRect> m_overrideVisibleRect;

    // Tile edge in tile-grid points. The zoomed-in grid's host layer carries no
    // transform (LegacyTileCache::adjustTileGridTransforms() only scales the
    // zoomed-out grid), so this space is screen points: a 320x480pt viewport.
    //
    // LegacyTileGrid::calculateCoverRect() inflates the visible rect by w/2 on
    // each side and by h on each side, so the grid must always be able to hold
    // 2w x 3h = 640 x 1440pt in portrait and 960 x 960pt in landscape.
    // LegacyTileGrid::centerTileGridOrigin() then tiles that with
    // ceil(w/T) x ceil(h/T) tiles:
    //
    //   T=512: 2x3 =  6 tiles, 1024x1536pt covered -> 71% more than the 640x1440 wanted
    //   T=320: 2x5 = 10 tiles,  640x1600pt covered -> 11% more (landscape 3x3, exact)
    //   T=256: 3x6 = 18 tiles,  768x1536pt covered -> 28% more
    //
    // 320 is the best fit because it equals the viewport width, so the cover
    // rect is a whole number of tiles across in both orientations.
    //
    // The eviction grain matters as much as the total. computeAvailableMemory()
    // rounds up to a 128MB multiple, so ramSize() reports 512MB here and
    // LegacyTileCache::tileCapacityForGrid() caps the cache at 24MB and floors
    // the active grid at 18MB, rather than letting systemMemoryLevel()
    // (kern.memorystatus_level, and -1 if that sysctl is unavailable) tier it
    // down to 6MB. LegacyTileGrid::tileByteSize() charges (T*screenScale)^2*4 per
    // tile flat, so at screenScale 2:
    //
    //   T=512: 4MB/tile      -> 24MB for the cover rect. At the 12MB floor the
    //          grid holds 3 of the 6 tiles it wants, so dropDistantTiles() and
    //          createTiles() fight and each round repaints 1024x1024px.
    //   T=320: 1.5625MB/tile -> 15.6MB for the cover rect, and 7 tiles still fit
    //          in the 12MB floor while the visible rect needs only 1x2 = 3.1MB.
    IntSize m_tileSize { 320, 320 };
    
    TilingMode m_tilingMode { Normal };
    TilingDirection m_tilingDirection { TilingDirectionDown };
    
    bool m_keepsZoomedOutTiles { false };
    bool m_hasPendingLayoutTiles { false };
    bool m_hasPendingUpdateTilingMode { false };
    bool m_tilesOpaque { true };
    bool m_tileBordersVisible { false };
    bool m_tilePaintCountersVisible { false };
    bool m_acceleratedDrawingEnabled { false };
    bool m_isSpeculativeTileCreationEnabled { true };
    bool m_tileControllerShouldUseLowScaleTiles { false };
    bool m_didCallWillStartScrollingOrZooming { false };
    
    std::unique_ptr<LegacyTileGrid> m_zoomedOutTileGrid;
    std::unique_ptr<LegacyTileGrid> m_zoomedInTileGrid;

    Timer m_tileCreationTimer;

    Vector<IntRect> m_savedDisplayRects;

    float m_currentScale { 1 };

    float m_pendingScale { 0 };
    float m_pendingZoomedOutScale { 0 };

    mutable Lock m_tileMutex;
    mutable Lock m_savedDisplayRectMutex;
    mutable Lock m_contentReplacementImageMutex;
};

} // namespace WebCore

#endif // PLATFORM(IOS_FAMILY)
