/*
 * Copyright (C) 2011-2024 Apple Inc. All rights reserved.
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
#import "LegacyTileGrid.h"

#if PLATFORM(IOS_FAMILY)

#import "LegacyTileGridTile.h"
#import "LegacyTileLayer.h"
#import "LegacyTileLayerPool.h"
#import "SystemMemory.h"
#import "WAKWindow.h"
#import <algorithm>
#import <functional>
#import <pal/spi/cg/CoreGraphicsSPI.h>
#import <pal/spi/cocoa/QuartzCoreSPI.h>
#import <ranges>
#import <wtf/MemoryPressureHandler.h>
#import <wtf/TZoneMallocInlines.h>

#if defined(WEBKIT_IOS6)
#include <unistd.h>
// Our own running commentary. WTFLogAlways reaches a file through stderr, so
// every one of these is a synchronous write on whichever thread the engine is
// on - and some of them sit on paths that run for every frame of a scroll.
static bool engineChatterEnabled()
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = access("/tmp/native-engine-log", F_OK) == 0 ? 1 : 0;
    return enabled == 1;
}
#endif


namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(LegacyTileGrid);

LegacyTileGrid::LegacyTileGrid(LegacyTileCache& tileCache, const IntSize& tileSize)
    : m_tileCache(tileCache)
    , m_tileHostLayer(adoptNS([[LegacyTileHostLayer alloc] initWithTileGrid:this]))
    , m_tileSize(tileSize)
    , m_scale(1)
    , m_validBounds(0, 0, std::numeric_limits<int>::max(), std::numeric_limits<int>::max()) 
{
}
    
LegacyTileGrid::~LegacyTileGrid()
{
    [m_tileHostLayer removeFromSuperlayer];
}

IntRect LegacyTileGrid::visibleRect() const
{
    Ref tileCache = m_tileCache.get();
    IntRect visibleRect = enclosingIntRect(tileCache->visibleRectInLayer(m_tileHostLayer.get()));

    // When fast scrolling to the top, move the visible rect there immediately so we have tiles when the scrolling completes.
    if (tileCache->tilingMode() == LegacyTileCache::ScrollToTop)
        visibleRect.setY(0);

    return visibleRect;
}

void LegacyTileGrid::dropAllTiles()
{
    m_tiles.clear();
}

void LegacyTileGrid::dropTilesIntersectingRect(const IntRect& dropRect)
{
    dropTilesBetweenRects(dropRect, IntRect());
}

void LegacyTileGrid::dropTilesOutsideRect(const IntRect& keepRect)
{
    dropTilesBetweenRects(IntRect(0, 0, std::numeric_limits<int>::max(), std::numeric_limits<int>::max()), keepRect);
}

void LegacyTileGrid::dropTilesBetweenRects(const IntRect& dropRect, const IntRect& keepRect)
{
    Vector<TileIndex, 16> toRemove;
    for (const auto& tile : m_tiles) {
        const TileIndex& index = tile.key;
        IntRect tileRect = tile.value->rect();
        if (tileRect.intersects(dropRect) && !tileRect.intersects(keepRect))
            toRemove.append(index);
    }
    unsigned removeCount = toRemove.size();
    for (unsigned n = 0; n < removeCount; ++n)
        m_tiles.remove(toRemove[n]);
}

unsigned LegacyTileGrid::tileByteSize() const
{
    IntSize tilePixelSize = m_tileSize;
    tilePixelSize.scale(protect(tileCache())->screenScale());
    return LegacyTileLayerPool::bytesBackingLayerWithPixelSize(tilePixelSize);
}

template <typename T>
static bool isFartherAway(const std::pair<double, T>& a, const std::pair<double, T>& b)
{
    return a.first > b.first;
}

bool LegacyTileGrid::dropDistantTiles(unsigned tilesNeeded, double shortestDistance, const IntRect& visibleRect)
{
    // One protective reference for both queries: taking it retains and releases
    // the window, and tileByteSize() would take a second one of its own.
    Ref tileCache = m_tileCache.get();
    IntSize tilePixelSize = m_tileSize;
    tilePixelSize.scale(tileCache->screenScale());
    unsigned bytesPerTile = LegacyTileLayerPool::bytesBackingLayerWithPixelSize(tilePixelSize);
    unsigned bytesNeeded = tilesNeeded * bytesPerTile;
    unsigned bytesUsed = tileCount() * bytesPerTile;
    unsigned maximumBytes = tileCache->tileCapacityForGrid(this);

    int bytesToReclaim = int(bytesUsed) - (int(maximumBytes) - bytesNeeded);
    if (bytesToReclaim <= 0)
        return true;

    unsigned tilesToRemoveCount = bytesToReclaim / bytesPerTile;

    const TileDistanceMetrics metrics = distanceMetricsFor(visibleRect);
    Vector<std::pair<double, TileIndex>, 16> toRemove;
    for (const auto& tile : m_tiles) {
        const TileIndex& index = tile.key;
        const IntRect& tileRect = tile.value->rect();
        double distance = tileDistance2(visibleRect, tileRect, metrics);
        if (distance <= shortestDistance)
            continue;
        toRemove.append(std::make_pair(distance, index));
        std::ranges::push_heap(toRemove, isFartherAway<TileIndex>);
        if (toRemove.size() > tilesToRemoveCount) {
            std::ranges::pop_heap(toRemove, isFartherAway<TileIndex>);
            toRemove.removeLast();
        }
    }
    size_t removeCount = toRemove.size();
    for (size_t n = 0; n < removeCount; ++n)
        m_tiles.remove(toRemove[n].second);

    if (!shortestDistance)
        return true;

    return tileCount() * bytesPerTile + bytesNeeded <= maximumBytes;
}

void LegacyTileGrid::addTilesCoveringRect(const IntRect& rectToCover)
{
    // We never draw anything outside of our bounds.
    const IntRect bounds = this->bounds();
    IntRect rect(rectToCover);
    rect.intersect(bounds);
    if (rect.isEmpty())
        return;

    TileIndex topLeftIndex = tileIndexForPoint(topLeft(rect));
    TileIndex bottomRightIndex = tileIndexForPoint(bottomRight(rect));
    for (int yIndex = topLeftIndex.y(); yIndex <= bottomRightIndex.y(); ++yIndex) {
        for (int xIndex = topLeftIndex.x(); xIndex <= bottomRightIndex.x(); ++xIndex) {
            TileIndex index(xIndex, yIndex);
            if (!m_tiles.contains(index))
                addTileForIndex(index, bounds);
        }
    }
}

void LegacyTileGrid::addTileForIndex(const TileIndex& index)
{
    addTileForIndex(index, bounds());
}

void LegacyTileGrid::addTileForIndex(const TileIndex& index, const IntRect& bounds)
{
    m_tiles.set(index, LegacyTileGridTile::create(this, tileRectForIndex(index, bounds)));
}

CALayer* LegacyTileGrid::tileHostLayer() const
{
    return m_tileHostLayer.get();
}

IntRect LegacyTileGrid::bounds() const
{
    return IntRect(IntPoint(), IntSize([tileHostLayer() size]));
}

RefPtr<LegacyTileGridTile> LegacyTileGrid::tileForIndex(const TileIndex& index) const
{
    return m_tiles.get(index);
}

IntRect LegacyTileGrid::tileRectForIndex(const TileIndex& index) const
{
    return tileRectForIndex(index, bounds());
}

// bounds() is a message send to the host layer. Callers that walk a range of
// indices read it once and pass it in.
IntRect LegacyTileGrid::tileRectForIndex(const TileIndex& index, const IntRect& bounds) const
{
    IntRect rect(index.x() * m_tileSize.width() - (m_origin.x() ? m_tileSize.width() - m_origin.x() : 0),
                 index.y() * m_tileSize.height() - (m_origin.y() ? m_tileSize.height() - m_origin.y() : 0),
                 m_tileSize.width(),
                 m_tileSize.height());
    rect.intersect(bounds);
    return rect;
}

LegacyTileGrid::TileIndex LegacyTileGrid::tileIndexForPoint(const IntPoint& point) const
{
    ASSERT(m_origin.x() < m_tileSize.width());
    ASSERT(m_origin.y() < m_tileSize.height());
    int x = (point.x() + (m_origin.x() ? m_tileSize.width() - m_origin.x() : 0)) / m_tileSize.width();
    int y = (point.y() + (m_origin.y() ? m_tileSize.height() - m_origin.y() : 0)) / m_tileSize.height();
    return TileIndex(std::max(x, 0), std::max(y, 0));
}

void LegacyTileGrid::centerTileGridOrigin(const IntRect& visibleRect)
{
    centerTileGridOrigin(visibleRect, bounds());
}

void LegacyTileGrid::centerTileGridOrigin(const IntRect& visibleRect, const IntRect& bounds)
{
    if (visibleRect.isEmpty())
        return;

    unsigned minimumHorizontalTiles = 1 + (visibleRect.width() - 1) / m_tileSize.width();
    unsigned minimumVerticalTiles = 1 + (visibleRect.height() - 1) / m_tileSize.height();
    TileIndex currentTopLeftIndex = tileIndexForPoint(topLeft(visibleRect));
    TileIndex currentBottomRightIndex = tileIndexForPoint(bottomRight(visibleRect));
    unsigned currentHorizontalTiles = currentBottomRightIndex.x() - currentTopLeftIndex.x() + 1;
    unsigned currentVerticalTiles = currentBottomRightIndex.y() - currentTopLeftIndex.y() + 1;

    // If we have tiles already, only center if we would get benefits from both directions (as we need to throw out existing tiles).
    if (tileCount() && (currentHorizontalTiles == minimumHorizontalTiles || currentVerticalTiles == minimumVerticalTiles)) {
        if (engineChatterEnabled()) WTFLogAlways("[center] already minimal: h %u/%u v %u/%u at y=%d",
            currentHorizontalTiles, minimumHorizontalTiles, currentVerticalTiles, minimumVerticalTiles, visibleRect.y());
        return;
    }

    IntPoint newOrigin(0, 0);
    IntSize size = bounds.size();
    if (size.width() > m_tileSize.width()) {
        newOrigin.setX((visibleRect.x() - (minimumHorizontalTiles * m_tileSize.width() - visibleRect.width()) / 2) % m_tileSize.width());
        if (newOrigin.x() < 0)
            newOrigin.setX(0);
    }
    if (size.height() > m_tileSize.height()) {
        newOrigin.setY((visibleRect.y() - (minimumVerticalTiles * m_tileSize.height() - visibleRect.height()) / 2) % m_tileSize.height());
        if (newOrigin.y() < 0)
            newOrigin.setY(0);
    }

    if (newOrigin == m_origin) {
        if (engineChatterEnabled()) WTFLogAlways("[center] visible y=%d h=%d origin unchanged at %d,%d tiles=%u",
            visibleRect.y(), visibleRect.height(), m_origin.x(), m_origin.y(), (unsigned)tileCount());
        return;
    }
    if (engineChatterEnabled()) WTFLogAlways("[center] visible y=%d h=%d origin %d,%d -> %d,%d (dropping %u tiles)",
        visibleRect.y(), visibleRect.height(), m_origin.x(), m_origin.y(),
        newOrigin.x(), newOrigin.y(), (unsigned)tileCount());
    m_tiles.clear();
    m_origin = newOrigin;
}

RefPtr<LegacyTileGridTile> LegacyTileGrid::tileForPoint(const IntPoint& point) const
{
    return tileForIndex(tileIndexForPoint(point));
}

bool LegacyTileGrid::tilesCover(const IntRect& rect) const
{
    return m_tiles.contains(tileIndexForPoint(rect.location()))
        && m_tiles.contains(tileIndexForPoint(IntPoint(rect.maxX() - 1, rect.y())))
        && m_tiles.contains(tileIndexForPoint(IntPoint(rect.x(), rect.maxY() - 1)))
        && m_tiles.contains(tileIndexForPoint(IntPoint(rect.maxX() - 1, rect.maxY() - 1)));
}

void LegacyTileGrid::updateTileOpacity()
{
    const BOOL opaque = m_tileCache->tilesOpaque();
    TileMap::iterator end = m_tiles.end();
    for (TileMap::iterator it = m_tiles.begin(); it != end; ++it)
        [it->value->tileLayer() setOpaque:opaque];
}

void LegacyTileGrid::updateTileBorderVisibility()
{
    const bool visible = protect(m_tileCache)->tileBordersVisible();
    TileMap::iterator end = m_tiles.end();
    for (TileMap::iterator it = m_tiles.begin(); it != end; ++it)
        it->value->showBorder(visible);
}

unsigned LegacyTileGrid::tileCount() const
{
    return m_tiles.size();
}

bool LegacyTileGrid::checkDoSingleTileLayout()
{
    const IntRect bounds = this->bounds();
    IntSize size = bounds.size();
    if (size.width() > m_tileSize.width() || size.height() > m_tileSize.height())
        return false;

    if (m_origin != IntPoint(0, 0)) {
        m_tiles.clear();
        m_origin = IntPoint(0, 0);
    }

    dropInvalidTiles(bounds);

    if (size.isEmpty()) {
        ASSERT(!m_tiles.get(TileIndex(0, 0)));
        return true;
    }

    TileIndex originIndex(0, 0);
    if (!m_tiles.get(originIndex))
        m_tiles.set(originIndex, LegacyTileGridTile::create(this, tileRectForIndex(originIndex, bounds)));

    return true;
}

void LegacyTileGrid::updateHostLayerSize()
{
    CALayer* hostLayer = protect(tileCache())->hostLayer();
    CGRect tileHostBounds = [hostLayer convertRect:[hostLayer bounds] toLayer:tileHostLayer()];
    CGSize transformedSize;
    transformedSize.width = CGRound(tileHostBounds.size.width);
    transformedSize.height = CGRound(tileHostBounds.size.height);

    CGRect bounds = [tileHostLayer() bounds];
    if (CGSizeEqualToSize(bounds.size, transformedSize))
        return;
    bounds.size = transformedSize;
    [tileHostLayer() setBounds:bounds];
}

void LegacyTileGrid::dropInvalidTiles()
{
    dropInvalidTiles(bounds());
}

void LegacyTileGrid::dropInvalidTiles(const IntRect& bounds)
{
    IntRect dropBounds = intersection(m_validBounds, bounds);
    Vector<TileIndex, 16> toRemove;
    for (const auto& tile : m_tiles) {
        const TileIndex& index = tile.key;
        const IntRect& tileRect = tile.value->rect();
        IntRect expectedTileRect = tileRectForIndex(index, bounds);
        if (expectedTileRect != tileRect || !dropBounds.contains(tileRect))
            toRemove.append(index);
    }
    unsigned removeCount = toRemove.size();
    for (unsigned n = 0; n < removeCount; ++n)
        m_tiles.remove(toRemove[n]);

    m_validBounds = bounds;
}

void LegacyTileGrid::invalidateTiles(const IntRect& dirtyRect)
{
    if (!hasTiles())
        return;

    IntRect bounds = this->bounds();
    if (intersection(bounds, m_validBounds) != m_validBounds) {
        // The bounds have got smaller. Everything outside will also be considered invalid and will be dropped by dropInvalidTiles().
        // Due to dirtyRect being limited to current bounds the tiles that are temporarily outside might miss invalidation 
        // completely othwerwise.
        m_validBounds = bounds;
    }

    Vector<TileIndex, 16> invalidatedTiles;

    if (dirtyRect.width() > m_tileSize.width() * 4 || dirtyRect.height() > m_tileSize.height() * 4) {
        // For large invalidates, iterate over live tiles.
        TileMap::iterator end = m_tiles.end();
        for (TileMap::iterator it = m_tiles.begin(); it != end; ++it) {
            LegacyTileGridTile& tile = it->value.get();
            if (!tile.rect().intersects(dirtyRect))
               continue;
            tile.invalidateRect(dirtyRect);
            invalidatedTiles.append(it->key);
        }
    } else {
        TileIndex topLeftIndex = tileIndexForPoint(topLeft(dirtyRect));
        TileIndex bottomRightIndex = tileIndexForPoint(bottomRight(dirtyRect));
        for (int yIndex = topLeftIndex.y(); yIndex <= bottomRightIndex.y(); ++yIndex) {
            for (int xIndex = topLeftIndex.x(); xIndex <= bottomRightIndex.x(); ++xIndex) {
                TileIndex index(xIndex, yIndex);
                LegacyTileGridTile* tile = m_tiles.get(index);
                if (!tile)
                    continue;
                if (!tile->rect().intersects(dirtyRect))
                    continue;
                tile->invalidateRect(dirtyRect);
                invalidatedTiles.append(index);
            }
        }
    }
    if (invalidatedTiles.isEmpty())
        return;
    // When using minimal coverage, drop speculative tiles instead of updating them.
    if (!shouldUseMinimalTileCoverage())
        return;
    if (m_tileCache->tilingMode() != LegacyTileCache::Minimal && m_tileCache->tilingMode() != LegacyTileCache::Normal)
        return;
    IntRect visibleRect = this->visibleRect();
    unsigned count = invalidatedTiles.size();
    for (unsigned i = 0; i < count; ++i) {
        LegacyTileGridTile* tile = m_tiles.get(invalidatedTiles[i]);
        if (tile && !tile->rect().intersects(visibleRect))
            m_tiles.remove(invalidatedTiles[i]);
    }
}

bool LegacyTileGrid::shouldUseMinimalTileCoverage() const
{
    bool minimalMode = m_tileCache->tilingMode() == LegacyTileCache::Minimal;
    bool noSpeculative = !m_tileCache->isSpeculativeTileCreationEnabled();
#if defined(WEBKIT_IOS6)
    // Only when the process is actually near the kill, not merely warned.
    //
    // A warning is the normal state here - the page's own memory keeps the
    // process above three quarters of its budget for as long as it is open - and
    // answering it by painting only what is on screen means a flick lands on
    // page that was laid out and never painted. Photographed over six flicks,
    // the longest unpainted run averaged 263 px of a 480 px screen and five
    // frames in six ended blank.
    static const bool coverageFollowsPolicy = !!getenv("WEBKIT_IOS6_MINIMAL_TILES");
    bool underPressure = coverageFollowsPolicy
        ? MemoryPressureHandler::singleton().isUnderMemoryPressure()
        : MemoryPressureHandler::singleton().memoryPressureStatus() == SystemMemoryPressureStatus::Critical;
#else
    bool underPressure = MemoryPressureHandler::singleton().isUnderMemoryPressure();
#endif

    static int lastReported = -1;
    int state = (minimalMode ? 1 : 0) | (noSpeculative ? 2 : 0) | (underPressure ? 4 : 0);
    if (state != lastReported) {
        lastReported = state;
        if (engineChatterEnabled()) WTFLogAlways("[tilecoverage] minimal=%d (tilingMode=%d speculativeOff=%d pressure=%d unused=%d)",
            state ? 1 : 0, (int)m_tileCache->tilingMode(), noSpeculative, underPressure, 0);
    }

    return minimalMode || noSpeculative || underPressure;
}

IntRect LegacyTileGrid::adjustCoverRectForPageBounds(const IntRect& rect, bool useMinimalCoverage) const
{
    // Adjust the rect so that it stays within the bounds and keeps the pixel size.
    IntRect bounds = this->bounds();
    IntRect adjustedRect = rect;
    adjustedRect.move(rect.x() < bounds.x() ? bounds.x() - rect.x() : 0,
              rect.y() < bounds.y() ? bounds.y() - rect.y() : 0);
    adjustedRect.move(rect.maxX() > bounds.maxX() ? bounds.maxX() - rect.maxX() : 0,
              rect.maxY() > bounds.maxY() ? bounds.maxY() - rect.maxY() : 0);
    adjustedRect = intersection(bounds, adjustedRect);
    if (adjustedRect == rect || adjustedRect.isEmpty() || useMinimalCoverage)
        return adjustedRect;
    int pixels = adjustedRect.width() * adjustedRect.height();
    if (adjustedRect.width() != rect.width())
        adjustedRect.inflateY((pixels / adjustedRect.width() - adjustedRect.height()) / 2);
    else if (adjustedRect.height() != rect.height())
        adjustedRect.inflateX((pixels / adjustedRect.height() - adjustedRect.width()) / 2);
    return intersection(adjustedRect, bounds);
}

IntRect LegacyTileGrid::calculateCoverRect(const IntRect& visibleRect, bool& centerGrid, bool useMinimalCoverage)
{
    if (useMinimalCoverage) {
        centerGrid = true;
        return visibleRect;
    }
    IntRect coverRect = visibleRect;
    centerGrid = false;

    // Two screens above and two below, in half-screen units.
    //
    // Half a screen each way was chosen when tiles were thought to be the
    // largest block of memory in the process. They are not: twenty runs of ten
    // 400 px flicks over the feed, photographed eight frames at a time, put peak
    // resident between 206 and 233 MB whatever this number was, because decoded
    // images move it far more than layers do.
    //
    // Nor is this number what leaves the feed unpainted. On a 24000 px static
    // page every setting from three tiles and 920 px to eleven tiles and 2300 px
    // painted every frame of the same ten flicks, longest unpainted run 14 px of
    // a 448 px content area. On the feed no setting painted reliably and the
    // spread between settings sat inside the spread between repeats of one
    // setting - 31 to 62% of the settled page for this value over five runs,
    // 22 to 56% for half a screen over four. What is left over the feed is the
    // site's own layout and script, at two to twelve frames per second with
    // pauses of twelve seconds; no depth of coverage outruns that.
    //
    // Two screens each way is kept because it costs nothing: paired with the
    // grid in LegacyTileCache::tileCapacityForGrid() it holds eleven tiles,
    // 17.2 MB, against the fourteen tiles and 21.9 MB that half a screen and the
    // old ceiling reached on the same page - the same memory spent ahead of the
    // reader instead of behind. Raising this without raising that grid is the
    // one thing measured as actively worse.
    //
    // Standing warning from the session that cut this to half a screen: eleven
    // or twelve tiles of layers took the process down four times in nine soaks
    // of eight rounds, twice inside the GPU driver. That is the count this pair
    // of numbers now settles at. Twenty five sessions since have exited cleanly,
    // but the soak has not been repeated - if deaths come back, this is where to
    // look first, and the two environment variables move it without a rebuild.
    static const int verticalScreens = [] -> int {
        if (const char* override = getenv("WEBKIT_IOS6_TILE_COVERAGE_HALVES")) {
            int value = atoi(override);
            if (value > 0 && value <= 8)
                return value;
        }
        return 4;
    }();
    coverRect.inflateX(visibleRect.width() / 2);
    coverRect.inflateY(visibleRect.height() * verticalScreens / 2);
    return adjustCoverRectForPageBounds(coverRect, false);
}

LegacyTileGrid::TileDistanceMetrics LegacyTileGrid::distanceMetricsFor(const IntRect& visibleRect) const
{
    double horizontalBias = 1.0;
    double leftwardBias = 1.0;
    double rightwardBias = 1.0;

    double verticalBias = 1.0;
    double upwardBias = 1.0;
    double downwardBias = 1.0;

    const double tilingBiasVeryLikely = 0.8;
    const double tilingBiasLikely = 0.9;

    switch (m_tileCache->tilingDirection()) {
    case LegacyTileCache::TilingDirectionUp:
        verticalBias = tilingBiasVeryLikely;
        upwardBias = tilingBiasLikely;
        break;
    case LegacyTileCache::TilingDirectionDown:
        verticalBias = tilingBiasVeryLikely;
        downwardBias = tilingBiasLikely;
        break;
    case LegacyTileCache::TilingDirectionLeft:
        horizontalBias = tilingBiasVeryLikely;
        leftwardBias = tilingBiasLikely;
        break;
    case LegacyTileCache::TilingDirectionRight:
        horizontalBias = tilingBiasVeryLikely;
        rightwardBias = tilingBiasLikely;
        break;
    }

    double aspectX = horizontalBias * visibleRect.height() / visibleRect.width();
    double aspectY = verticalBias * visibleRect.width() / visibleRect.height();

    return TileDistanceMetrics {
        visibleRect.location() + IntSize(visibleRect.width() / 2, visibleRect.height() / 2),
        aspectX * leftwardBias,
        aspectX * rightwardBias,
        aspectY * upwardBias,
        aspectY * downwardBias
    };
}

// The "distance" calculated here is used to pick which tile to cache next. The idea is to create those
// closest to the current viewport first so the user is more likely to see already rendered content we she
// scrolls. The calculation is weighted to prefer vertical and downward direction.
double LegacyTileGrid::tileDistance2(const IntRect& visibleRect, const IntRect& tileRect, const TileDistanceMetrics& metrics)
{
    if (visibleRect.intersects(tileRect))
        return 0;
    IntPoint tileCenter = tileRect.location() + IntSize(tileRect.width() / 2, tileRect.height() / 2);

    int dx = tileCenter.x() - metrics.visibleCenter.x();
    int dy = tileCenter.y() - metrics.visibleCenter.y();

    double xDistance = (dx >= 0 ? metrics.xScaleRightward : metrics.xScaleLeftward) * dx;
    double yDistance = (dy >= 0 ? metrics.yScaleDownward : metrics.yScaleUpward) * dy;

    return xDistance * xDistance + yDistance * yDistance;
}

void LegacyTileGrid::createTiles(LegacyTileCache::SynchronousTileCreationMode creationMode)
{
    if (engineChatterEnabled()) {
        static MonotonicTime lastReport;
        MonotonicTime now = MonotonicTime::now();
        if (now - lastReport > 1_s) {
            lastReport = now;
            IntRect reportedVisible = visibleRect();
            WTFLogAlways("[tiles] creating for visible %d,%d %dx%d, %u tiles, mode %d, minimal %d",
                reportedVisible.x(), reportedVisible.y(), reportedVisible.width(), reportedVisible.height(),
                (unsigned)m_tiles.size(), (int)creationMode, shouldUseMinimalTileCoverage());
        }
    }

    IntRect visibleRect = this->visibleRect();
    if (visibleRect.isEmpty())
        return;

    // bounds() and shouldUseMinimalTileCoverage() are a layer message send and a
    // memory-pressure query respectively, and neither changes inside one pass.
    IntRect bounds = this->bounds();
    const bool coverRectIsOnlyTheViewport = shouldUseMinimalTileCoverage();

    // Drop tiles that are wrong size or outside the frame (because the frame has been resized).
    dropInvalidTiles(bounds);

    bool centerGrid;
    IntRect coverRect = calculateCoverRect(visibleRect, centerGrid, coverRectIsOnlyTheViewport);

    // If tile size is bigger than the view, centering minimizes the painting needed to cover the screen.
    // This is especially useful after zooming
    centerGrid = centerGrid || !tileCount();
    if (centerGrid)
        centerTileGridOrigin(visibleRect, bounds);

    double shortestDistance = std::numeric_limits<double>::infinity();
    double coveredDistance = 0;
    Vector<LegacyTileGrid::TileIndex, 16> tilesToCreate;
    unsigned pendingTileCount = 0;

    const TileDistanceMetrics metrics = distanceMetricsFor(visibleRect);

    LegacyTileGrid::TileIndex topLeftIndex = tileIndexForPoint(topLeft(coverRect));
    LegacyTileGrid::TileIndex bottomRightIndex = tileIndexForPoint(bottomRight(coverRect));
    // tileRectForIndex() reduces to two multiplies once the grid origin is
    // folded in, and the row's y coordinate does not change across a row.
    const int tileWidth = m_tileSize.width();
    const int tileHeight = m_tileSize.height();
    const int xOriginOffset = m_origin.x() ? tileWidth - m_origin.x() : 0;
    const int yOriginOffset = m_origin.y() ? tileHeight - m_origin.y() : 0;
    for (int yIndex = topLeftIndex.y(); yIndex <= bottomRightIndex.y(); ++yIndex) {
        const int tileY = yIndex * tileHeight - yOriginOffset;
        for (int xIndex = topLeftIndex.x(); xIndex <= bottomRightIndex.x(); ++xIndex) {
            LegacyTileGrid::TileIndex index(xIndex, yIndex);
            IntRect tileRect(xIndex * tileWidth - xOriginOffset, tileY, tileWidth, tileHeight);
            tileRect.intersect(bounds);
            // Currently visible tiles have distance of 0 and get all created in the same transaction.
            double distance = tileDistance2(visibleRect, tileRect, metrics);
            if (distance > coveredDistance)
                coveredDistance = distance;
            if (m_tiles.contains(index))
                continue;
            ++pendingTileCount;
            if (coverRectIsOnlyTheViewport) {
                shortestDistance = 0;
                tilesToCreate.append(index);
                continue;
            }
            if (distance > shortestDistance)
                continue;
            if (distance < shortestDistance) {
                tilesToCreate.clear();
                shortestDistance = distance;
            }
            tilesToCreate.append(index);
        }
    }

    size_t tilesToCreateCount = tilesToCreate.size();

    // Tile creation timer will invoke this function again in CoverSpeculative mode.
    bool candidateTilesAreSpeculative = shortestDistance > 0;
    if (creationMode == LegacyTileCache::CoverVisibleOnly && candidateTilesAreSpeculative)
        tilesToCreateCount = 0;

    // Even if we don't create any tiles, we should still drop distant tiles
    // in case coverRect got smaller.
    double keepDistance = std::min(shortestDistance, coveredDistance);
    if (!dropDistantTiles(tilesToCreateCount, keepDistance, visibleRect))
        return;

    ASSERT(pendingTileCount >= tilesToCreateCount);
    if (!pendingTileCount)
        return;

    for (size_t n = 0; n < tilesToCreateCount; ++n)
        addTileForIndex(tilesToCreate[n], bounds);

    bool didCreateTiles = !!tilesToCreateCount;
    bool createMoreTiles = pendingTileCount > tilesToCreateCount;

    static unsigned reportTick = 0;
    if (engineChatterEnabled() && !(reportTick++ % 8)) {
        WTFLogAlways("[tiles] %u tiles of %dx%d (%.1f MB each) = %.1f MB, coverRect %dx%d, doc %dx%d",
            (unsigned)m_tiles.size(), m_tileSize.width(), m_tileSize.height(),
            tileByteSize() / (1024.0 * 1024.0),
            (double)m_tiles.size() * tileByteSize() / (1024.0 * 1024.0),
            coverRect.width(), coverRect.height(),
            tileCache().tileControllerShouldUseLowScaleTiles() ? 0 : bounds.width(), bounds.height());
    }

    protect(tileCache())->finishedCreatingTiles(didCreateTiles, createMoreTiles);
}

void LegacyTileGrid::dumpTiles()
{
    IntRect visibleRect = this->visibleRect();
    NSLog(@"transformed visibleRect = [%6d %6d %6d %6d]", visibleRect.x(), visibleRect.y(), visibleRect.width(), visibleRect.height());
    unsigned i = 0;
    TileMap::iterator end = m_tiles.end();
    for (TileMap::iterator it = m_tiles.begin(); it != end; ++it) {
        TileIndex& index = it->key;
        IntRect tileRect = it->value->rect();
        NSLog(@"#%-3d (%3d %3d) - [%6d %6d %6d %6d]%@", ++i, index.x(), index.y(), tileRect.x(), tileRect.y(), tileRect.width(), tileRect.height(), tileRect.intersects(visibleRect) ? @" *" : @"");
        NSLog(@"     %@", [it->value->tileLayer() contents]);
    }
}

} // namespace WebCore

#endif // PLATFORM(IOS_FAMILY)
