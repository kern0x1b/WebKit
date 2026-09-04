/*
 * Copyright (C) 2017 Apple Inc. All rights reserved.
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

#include "config.h"
#include "FastMallocAlignedMemoryAllocator.h"

#include "MarkedBlock.h"
#include <wtf/FastMalloc.h>

#if defined(WEBKIT_IOS6)
#include "Ios6BlockReservationPool.h"
#endif

namespace JSC {

FastMallocAlignedMemoryAllocator::FastMallocAlignedMemoryAllocator()
#if ENABLE(MALLOC_HEAP_BREAKDOWN)
    : m_heap("WebKit FastMallocAlignedMemoryAllocator")
#endif
{
}

FastMallocAlignedMemoryAllocator::~FastMallocAlignedMemoryAllocator() = default;

void* FastMallocAlignedMemoryAllocator::tryAllocateAlignedMemory(size_t alignment, size_t size)
{
#if ENABLE(MALLOC_HEAP_BREAKDOWN)
    return m_heap.memalign(alignment, size, true);
#elif defined(WEBKIT_IOS6)
    // MarkedBlock is the only caller of tryAllocateAlignedMemory (MarkedBlock.cpp), always
    // with alignment == size == MarkedBlock::blockSize. Route that traffic through the pool
    // shared with GigacageAlignedMemoryAllocator and StructureAlignedMemoryAllocator; fall
    // back to the plain allocator for anything else this allocator might ever be asked for.
    if (alignment == MarkedBlock::blockSize && size == MarkedBlock::blockSize)
        return Ios6BlockReservationPool::singleton().tryAllocateBlock();
    return tryFastCompactAlignedMalloc(alignment, size);
#else
    return tryFastCompactAlignedMalloc(alignment, size);
#endif

}

void FastMallocAlignedMemoryAllocator::freeAlignedMemory(void* basePtr)
{
#if ENABLE(MALLOC_HEAP_BREAKDOWN)
    return m_heap.free(basePtr);
#elif defined(WEBKIT_IOS6)
    Ios6BlockReservationPool::singleton().freeBlock(basePtr);
#else
    fastFree(basePtr);
#endif

}

void FastMallocAlignedMemoryAllocator::dump(PrintStream& out) const
{
    out.print("FastMalloc");
}

void* FastMallocAlignedMemoryAllocator::tryAllocateMemory(size_t size)
{
#if ENABLE(MALLOC_HEAP_BREAKDOWN)
    return m_heap.malloc(size);
#else
    return FastCompactMalloc::tryMalloc(size);
#endif
}

void FastMallocAlignedMemoryAllocator::freeMemory(void* pointer)
{
#if ENABLE(MALLOC_HEAP_BREAKDOWN)
    return m_heap.free(pointer);
#else
    FastCompactMalloc::free(pointer);
#endif
}

void* FastMallocAlignedMemoryAllocator::tryReallocateMemory(void* pointer, size_t size)
{
#if ENABLE(MALLOC_HEAP_BREAKDOWN)
    return m_heap.realloc(pointer, size);
#else
    return FastCompactMalloc::tryRealloc(pointer, size);
#endif
}

} // namespace JSC

