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
#import "LegacyTileLayerPool.h"

#if PLATFORM(IOS_FAMILY)

#import "LegacyTileGrid.h"
#import "LegacyTileLayer.h"
#import "Logging.h"
#import <wtf/MemoryPressureHandler.h>
#import <wtf/NeverDestroyed.h>
#import <wtf/darwin/DispatchExtras.h>

namespace WebCore {

static const Seconds capacityDecayTime { 5_s };

#if defined(WEBKIT_IOS6)
static unsigned legacyTileLayerPoolDefaultCapacity()
{
    static const unsigned capacity = [] -> unsigned {
        if (const char* override = getenv("WEBKIT_IOS6_TILE_LAYER_POOL_MB")) {
            int value = atoi(override);
            if (value >= 0 && value <= 64)
                return static_cast<unsigned>(value) * 1024 * 1024;
        }
        return 12 * 1024 * 1024;
    }();
    return capacity;
}
#endif

LegacyTileLayerPool::LegacyTileLayerPool()
    : m_totalBytes(0)
#if defined(WEBKIT_IOS6)
    , m_capacity(legacyTileLayerPoolDefaultCapacity())
#else
    , m_capacity(0)
#endif
    , m_needsPrune(false)
{
}

LegacyTileLayerPool* LegacyTileLayerPool::sharedPool()
{
    static NeverDestroyed<LegacyTileLayerPool> sharedPool;
    return &sharedPool.get();
}

unsigned LegacyTileLayerPool::bytesBackingLayerWithPixelSize(const IntSize& size)
{
    return size.area() * 4;
}

LegacyTileLayerPool::LayerList& LegacyTileLayerPool::listOfLayersWithSize(const IntSize& size, AccessType accessType)
{
    ASSERT(!m_layerPoolMutex.tryLock());
    HashMap<IntSize, LayerList>::iterator it = m_reuseLists.find(size);
    if (it == m_reuseLists.end()) {
        it = m_reuseLists.add(size, LayerList()).iterator;
        m_sizesInPruneOrder.append(size);
    } else if (accessType == MarkAsUsed) {
        m_sizesInPruneOrder.removeLast(size);
        m_sizesInPruneOrder.append(size);
    }
    return it->value;
}

void LegacyTileLayerPool::addLayer(const RetainPtr<LegacyTileLayer>& layer)
{
    IntSize layerSize([layer frame].size);
    layerSize.scale([layer contentsScale]);
    if (!canReuseLayerWithSize(layerSize))
        return;

#if defined(WEBKIT_IOS6)
    // Refusing the pool outright turns it off permanently here.
    //
    // isUnderMemoryPressure() is true whenever the footprint policy is Strict,
    // and this process sits there for as long as a page is open - the same
    // reading that made LegacyTileGrid::shouldUseMinimalTileCoverage() ask for
    // Critical instead. With the pool refusing every returned layer, each step
    // of a scroll destroys a tile layer and allocates a fresh one, and its
    // backing store is a 640x640x4 buffer the kernel has to zero.
    //
    // LegacyTileGrid::createTiles() drops distant tiles and then creates the new
    // ones in the same pass, so the layers a scroll needs are handed back
    // moments before they are asked for again: a couple of tiles' worth of pool
    // captures nearly all of the reuse. Under pressure the pool is kept to that,
    // rather than to nothing, and prune() still walks it down from there.
    const bool underPressure = MemoryPressureHandler::singleton().isUnderMemoryPressure();

    Locker locker { m_layerPoolMutex };

    if (underPressure) {
        const unsigned capacityUnderPressure = 4 * 1024 * 1024;
        if (m_totalBytes + bytesBackingLayerWithPixelSize(layerSize) > capacityUnderPressure)
            return;
    }
#else
    if (MemoryPressureHandler::singleton().isUnderMemoryPressure()) {
        LOG(MemoryPressure, "Under memory pressure: %s, totalBytes: %d", __PRETTY_FUNCTION__, m_totalBytes);
        return;
    }

    Locker locker { m_layerPoolMutex };
#endif
    listOfLayersWithSize(layerSize).prepend(layer);
    m_totalBytes += bytesBackingLayerWithPixelSize(layerSize);

    m_lastAddTime = WallTime::now();
    schedulePrune();
}

RetainPtr<LegacyTileLayer> LegacyTileLayerPool::takeLayerWithSize(const IntSize& size)
{
    if (!canReuseLayerWithSize(size))
        return nil;
    Locker locker { m_layerPoolMutex };
    LayerList& reuseList = listOfLayersWithSize(size, MarkAsUsed);
    if (reuseList.isEmpty())
        return nil;
    m_totalBytes -= bytesBackingLayerWithPixelSize(size);
    return reuseList.takeFirst();
}

void LegacyTileLayerPool::setCapacity(unsigned capacity)
{
    Locker reuseLocker { m_layerPoolMutex };
    if (capacity < m_capacity)
        schedulePrune();
    m_capacity = capacity;
}
    
unsigned LegacyTileLayerPool::decayedCapacity() const
{
    // Decay to one quarter over capacityDecayTime
    Seconds timeSinceLastAdd = WallTime::now() - m_lastAddTime;
    if (timeSinceLastAdd > capacityDecayTime)
        return m_capacity / 4;
    float decayProgess = float(timeSinceLastAdd / capacityDecayTime);
    return m_capacity / 4 + m_capacity * 3 / 4 * (1.f - decayProgess);
}

void LegacyTileLayerPool::schedulePrune()
{
    ASSERT(!m_layerPoolMutex.tryLock());
    if (m_needsPrune)
        return;
    m_needsPrune = true;
    dispatch_time_t nextPruneTime = dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC);
    dispatch_after(nextPruneTime, globalDispatchQueueSingleton(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        prune();
    });
}

void LegacyTileLayerPool::prune()
{
    Locker locker { m_layerPoolMutex };
    ASSERT(m_needsPrune);
    m_needsPrune = false;
    unsigned shrinkTo = decayedCapacity();
    while (m_totalBytes > shrinkTo) {
        ASSERT(!m_sizesInPruneOrder.isEmpty());
        IntSize sizeToDrop = m_sizesInPruneOrder.first();
        LayerList& oldestReuseList = m_reuseLists.find(sizeToDrop)->value;
        if (oldestReuseList.isEmpty()) {
            m_reuseLists.remove(sizeToDrop);
            m_sizesInPruneOrder.removeAt(0);
            continue;
        }
#if LOG_TILING
        NSLog(@"dropping layer of size %d x %d", sizeToDrop.width(), sizeToDrop.height());
#endif
        m_totalBytes -= bytesBackingLayerWithPixelSize(sizeToDrop);
        // The last element in the list is the oldest, hence most likely not to
        // still have a backing store.
        oldestReuseList.removeLast();
    }
    if (WallTime::now() - m_lastAddTime <= capacityDecayTime)
        schedulePrune();
}

void LegacyTileLayerPool::drain()
{
    Locker reuseLocker { m_layerPoolMutex };
    m_reuseLists.clear();
    m_sizesInPruneOrder.clear();
    m_totalBytes = 0;
}

} // namespace WebCore

#endif // PLATFORM(IOS_FAMILY)
