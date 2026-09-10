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

class Ios6BlockReservationPool {
    WTF_MAKE_NONCOPYABLE(Ios6BlockReservationPool);
public:
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
