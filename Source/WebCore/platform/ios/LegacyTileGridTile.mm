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

#import "config.h"
#import "LegacyTileGridTile.h"

#if PLATFORM(IOS_FAMILY)

#import "Color.h"
#import "ContentsFormatCocoa.h"
#import "IOSurface.h"
#import "LegacyTileCache.h"
#import "LegacyTileGrid.h"
#import "LegacyTileLayer.h"
#import "LegacyTileLayerPool.h"
#import "PlatformCALayer.h"
#import "WAKWindow.h"
#import <algorithm>
#import <functional>
#import <pal/spi/cocoa/QuartzCoreSPI.h>

namespace WebCore {

#if LOG_TILING
static int totalTileCount;
#endif

LegacyTileGridTile::LegacyTileGridTile(LegacyTileGrid* tileGrid, const IntRect& tileRect)
    : m_tileGrid(tileGrid)
    , m_rect(tileRect)
{
    ASSERT(!tileRect.isEmpty());
    IntSize pixelSize(m_rect.size());
    Ref tileCache = m_tileGrid->tileCache();
    const CGFloat screenScale = tileCache->screenScale();
    pixelSize.scale(screenScale);
    m_tileLayer = LegacyTileLayerPool::sharedPool()->takeLayerWithSize(pixelSize);
    if (!m_tileLayer) {
#if LOG_TILING
        NSLog(@"unable to reuse layer with size %d x %d, creating one", pixelSize.width(), pixelSize.height());
#endif
        m_tileLayer = adoptNS([[LegacyTileLayer alloc] init]);
    }
    LegacyTileLayer* layer = m_tileLayer.get();

    // Most of these layers come back out of the pool, where this same code put
    // them, so the values are already what they are about to be set to. A CA
    // setter is a write into the layer's transaction and, for contentsScale and
    // contentsFormat, an invalidation of its backing store; a getter is not. Only
    // the differences are written.
    if (NSString *formatString = contentsFormatString(PlatformCALayer::contentsFormatForLayer())) {
        if (![formatString isEqualToString:layer.contentsFormat])
            layer.contentsFormat = formatString;
    }

    [layer setTileGrid:tileGrid];
    const BOOL opaque = tileCache->tilesOpaque();
    if ([layer isOpaque] != opaque)
        [layer setOpaque:opaque];
    if ([layer edgeAntialiasingMask])
        [layer setEdgeAntialiasingMask:0];
    if ([layer needsLayoutOnGeometryChange])
        [layer setNeedsLayoutOnGeometryChange:NO];
    if ([layer contentsScale] != screenScale)
        [layer setContentsScale:screenScale];
    const BOOL drawsAsynchronously = tileCache->acceleratedDrawingEnabled();
    if ([layer drawsAsynchronously] != drawsAsynchronously)
        [layer setDrawsAsynchronously:drawsAsynchronously];

    // Host layer may have other sublayers. Keep the tile layers at the beginning of the array
    // so they are painted behind everything else.
    [tileGrid->tileHostLayer() insertSublayer:layer atIndex:tileGrid->tileCount()];
    [layer setFrame:m_rect];
    invalidateRect(m_rect);
    showBorder(tileCache->tileBordersVisible());

#if LOG_TILING
    ++totalTileCount;
    NSLog(@"new Tile (%d,%d) %d %d, count %d", tileRect.x(), tileRect.y(), tileRect.width(), tileRect.height(), totalTileCount);
#endif
}

LegacyTileGridTile::~LegacyTileGridTile() 
{
    [tileLayer() setTileGrid:0];
    [tileLayer() removeFromSuperlayer];
    LegacyTileLayerPool::sharedPool()->addLayer(tileLayer());
#if LOG_TILING
    --totalTileCount;
    NSLog(@"delete Tile (%d,%d) %d %d, count %d", m_rect.x(), m_rect.y(), m_rect.width(), m_rect.height(), totalTileCount);
#endif
}

void LegacyTileGridTile::invalidateRect(const IntRect& windowDirtyRect)
{
    IntRect dirtyRect = intersection(windowDirtyRect, m_rect);
    if (dirtyRect.isEmpty())
        return;
    dirtyRect.move(IntPoint() - m_rect.location());
    [tileLayer() setNeedsDisplayInRect:dirtyRect];

    if (m_tileGrid->tileCache().tilePaintCountersVisible())
        [tileLayer() setNeedsDisplayInRect:CGRectMake(0, 0, 46, 25)];
}

void LegacyTileGridTile::setRect(const IntRect& tileRect)
{
    if (m_rect == tileRect)
        return;
    m_rect = tileRect;
    LegacyTileLayer* layer = m_tileLayer.get();
    [layer setFrame:m_rect];
    [layer setNeedsDisplay];
}

void LegacyTileGridTile::showBorder(bool flag)
{
    LegacyTileLayer* layer = m_tileLayer.get();
    if (flag) {
        [layer setBorderColor:cachedCGColor(protect(m_tileGrid->tileCache())->colorForGridTileBorder(m_tileGrid)).get()];
        [layer setBorderWidth:0.5f];
    } else {
        if (!layer.borderWidth && !layer.borderColor)
            return;
        [layer setBorderColor:nil];
        [layer setBorderWidth:0];
    }
}

} // namespace WebCore

#endif // PLATFORM(IOS_FAMILY)
