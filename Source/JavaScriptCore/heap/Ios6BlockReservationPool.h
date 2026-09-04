/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if defined(WEBKIT_IOS6)

#include "MarkedBlock.h"
#include <wtf/BitVector.h>
#include <wtf/Lock.h>
#include <wtf/Locker.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Noncopyable.h>
#include <wtf/OSAllocator.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC {

// Shared block source for FastMallocAlignedMemoryAllocator, GigacageAlignedMemoryAllocator
// and StructureAlignedMemoryAllocator on this port, for their MarkedBlock::blockSize
// (16 KB) aligned allocations only. PreciseAllocation traffic -- tryAllocateMemory(),
// freeMemory(), tryReallocateMemory(), all arbitrary-size -- is untouched and still goes
// straight to each allocator's own backing malloc (FastMalloc, Gigacage, or, for
// Structure, RELEASE_ASSERT_NOT_REACHED(), since Structures never take that path).
//
// Why the three can share one pool: each independently called its own aligned malloc
// once per 16 KB block -- tryFastCompactAlignedMalloc() for FastMalloc,
// Gigacage::tryAlignedMalloc() for Gigacage, a per-block OSAllocator commit for
// Structure -- and each such call is free to land its backing pages in a distinct VM
// region. Under fragmentation that is one region-table entry per live block, times
// three allocators that never share space with one another even when a block from one
// is freed right next to where another wants to allocate. This pool instead reserves
// address space in 2 MB granules (128 blocks) up front -- uncommitted, so an unused
// granule costs no resident memory -- and commits/decommits individual 16 KB pages
// within a granule as blocks are handed out and returned. One granule backs up to 128
// blocks from any of the three allocators, so it is one VM region no matter how the
// live blocks are split between them.
//
// Why Structure blocks are safe to mix into the same reservation as FastMalloc/Gigacage
// blocks *on this port specifically*: StructureID encoding only needs all Structure
// blocks to share one aligned address range, with a constant top 32 bits, on
// CPU(ADDRESS64) -- see StructureAlignedMemoryAllocator::computePreferredStructureHeapReservationSize()
// and the CPU(ADDRESS64) branch of initializeStructureAddressSpace(). This is a 32-bit
// (!CPU(ADDRESS64)) port: its initializeStructureAddressSpace() sets startOfStructureHeap
// and structureIDBase to 0 and sizeOfStructureHeap to UINTPTR_MAX, i.e. there was never a
// reserved Structure address range to begin with here, and StructureID does not encode
// a Structure block's address at all on this configuration. There is nothing for sharing
// the pool to violate.
class Ios6BlockReservationPool {
    WTF_MAKE_NONCOPYABLE(Ios6BlockReservationPool);
public:
    // Public so NeverDestroyed<Ios6BlockReservationPool> can construct it in singleton()
    // below; this is the only construction path anyone is meant to use.
    Ios6BlockReservationPool() = default;

    static Ios6BlockReservationPool& singleton()
    {
        static NeverDestroyed<Ios6BlockReservationPool> instance;
        return instance.get();
    }

    void* tryAllocateBlock()
    {
        Locker locker(m_lock);

        for (auto& granule : m_granules) {
            size_t freeIndex = granule.usedBlocks.findBit(0, false);
            if (freeIndex < blocksPerGranule) {
                granule.usedBlocks.set(freeIndex);
                uint8_t* block = static_cast<uint8_t*>(granule.base) + freeIndex * MarkedBlock::blockSize;
                OSAllocator::commit(block, MarkedBlock::blockSize, true, false);
                return block;
            }
        }

        if (!addGranuleLocked())
            return nullptr;

        Granule& granule = m_granules.last();
        granule.usedBlocks.set(0);
        OSAllocator::commit(granule.base, MarkedBlock::blockSize, true, false);
        return granule.base;
    }

    void freeBlock(void* block)
    {
        Locker locker(m_lock);

        for (auto& granule : m_granules) {
            uintptr_t base = reinterpret_cast<uintptr_t>(granule.base);
            uintptr_t candidate = reinterpret_cast<uintptr_t>(block);
            if (candidate < base || candidate >= base + granuleSize)
                continue;

            OSAllocator::decommit(block, MarkedBlock::blockSize);
            size_t index = (candidate - base) / MarkedBlock::blockSize;
            granule.usedBlocks.quickClear(index);
            return;
        }

        RELEASE_ASSERT_NOT_REACHED();
    }

private:
    static constexpr size_t granuleSize = 2 * MB;
    static constexpr size_t blocksPerGranule = granuleSize / MarkedBlock::blockSize;

    struct Granule {
        void* base { nullptr };
        BitVector usedBlocks;
    };

    bool addGranuleLocked()
    {
        void* base = OSAllocator::tryReserveUncommittedAligned(granuleSize, granuleSize);
        if (!base)
            return false;

        Granule granule;
        granule.base = base;
        granule.usedBlocks.ensureSize(blocksPerGranule);
        m_granules.append(WTF::move(granule));
        return true;
    }

    Lock m_lock;
    Vector<Granule> m_granules;
};

} // namespace JSC

#endif // defined(WEBKIT_IOS6)
