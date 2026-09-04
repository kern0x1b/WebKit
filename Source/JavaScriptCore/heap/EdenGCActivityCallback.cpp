/*
 * Copyright (C) 2014-2026 Apple Inc. All rights reserved.
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
#include "EdenGCActivityCallback.h"

#include "VM.h"

namespace JSC {

EdenGCActivityCallback::EdenGCActivityCallback(JSC::Heap& heap, Synchronousness synchronousness)
    : GCActivityCallback(heap, synchronousness)
{
}

EdenGCActivityCallback::~EdenGCActivityCallback() = default;

void EdenGCActivityCallback::doCollection(VM& vm)
{
    setDidGCRecently(false);
#if defined(WEBKIT_IOS6)
    if (vm.heap.consumeEdenAllocationFloorSkip(0)) {
        // Reschedule to the time remaining until the skip episode's deadline, not to a flat
        // edenFloorRescheduleSeconds() every time. scheduleTimer can only shorten the timer's
        // delay (see its comment); didAllocate keeps shortening it too as the mutator keeps
        // allocating, so a flat reschedule request is silently dropped whenever didAllocate has
        // already pulled the delay below it, and the timer fires again almost immediately
        // instead of waiting. The remaining-time-to-deadline value only shrinks as real time
        // passes, so scheduleTimer can always honor it - if we get called again before the
        // deadline, we're just re-clamping to a smaller number.
        scheduleTimer(vm.heap.edenAllocationFloorSkipRemaining());
        return;
    }
    vm.heap.noteEdenActivityCallbackFired();
#endif
    vm.heap.collect(m_synchronousness, CollectionScope::Eden);
}

Seconds EdenGCActivityCallback::lastGCLength(JSC::Heap& heap)
{
    return heap.lastEdenGCLength();
}

double EdenGCActivityCallback::deathRate(JSC::Heap& heap)
{
    size_t sizeBefore = heap.sizeBeforeLastEdenCollection();
    size_t sizeAfter = heap.sizeAfterLastEdenCollection();
    return GCActivityCallback::deathRate(sizeBefore, sizeAfter);
}

double EdenGCActivityCallback::gcTimeSlice(size_t bytes)
{
    return std::min((static_cast<double>(bytes) / MB) * Options::percentCPUPerMBForEdenTimer(), Options::collectionTimerMaxPercentCPU());
}

} // namespace JSC
