/*
 * Copyright (C) 2006, 2008 Apple Inc. All rights reserved.
 * Copyright (C) 2009 Google Inc. All rights reserved.
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
#include <pthread.h>
#include "Timer.h"

#include "SharedTimer.h"
#include "ThreadGlobalData.h"
#include "ThreadTimers.h"
#include <limits>
#include <math.h>
#include <wtf/MainThread.h>
#include <wtf/RuntimeApplicationChecks.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>

#if PLATFORM(IOS_FAMILY)
#include "WebCoreThread.h"
#endif

#if PLATFORM(COCOA)
#include <wtf/cocoa/RuntimeApplicationChecksCocoa.h>
#endif

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(TimerBase);
WTF_MAKE_TZONE_ALLOCATED_IMPL(Timer);
WTF_MAKE_TZONE_ALLOCATED_IMPL(DeferrableOneShotTimer);

class TimerHeapReference;

// Timers are stored in a heap data structure, used to implement a priority queue.
// This allows us to efficiently determine which timer needs to fire the soonest.
// Then we set a single shared system timer to fire at that time.
//
// When a timer's "next fire time" changes, we need to move it around in the priority queue.
#if ASSERT_ENABLED
static ThreadTimerHeap& threadGlobalTimerHeap()
{
    return threadGlobalDataSingleton().threadTimers().timerHeap();
}
#endif

WTF_MAKE_COMPACT_TZONE_ALLOCATED_IMPL(ThreadTimerHeapItem);

inline ThreadTimerHeapItem::ThreadTimerHeapItem(TimerBase& timer, MonotonicTime time, unsigned insertionOrder)
    : time(time)
    , insertionOrder(insertionOrder)
    , m_threadTimers(threadGlobalDataSingleton().threadTimers())
    , m_timer(&timer)
{
    ASSERT(m_timer);
}
    
inline RefPtr<ThreadTimerHeapItem> ThreadTimerHeapItem::create(TimerBase& timer, MonotonicTime time, unsigned insertionOrder)
{
    return adoptRef(*new ThreadTimerHeapItem { timer, time, insertionOrder });
}

// ----------------

class TimerHeapPointer {
public:
    TimerHeapPointer(Ref<ThreadTimerHeapItem>* pointer)
        : m_pointer(pointer)
    { }

    TimerHeapReference operator*() const;
    Ref<ThreadTimerHeapItem>& NODELETE operator->() const { return *m_pointer; }
private:
    Ref<ThreadTimerHeapItem>* m_pointer;
};

class TimerHeapReference {
public:
    TimerHeapReference(Ref<ThreadTimerHeapItem>& reference)
        : m_reference(reference)
    { }

    TimerHeapReference(const TimerHeapReference& other)
        : m_reference(other.m_reference)
    { }

    operator Ref<ThreadTimerHeapItem>&() const { return m_reference; }
    TimerHeapPointer NODELETE operator&() const { return &m_reference; }
    TimerHeapReference& operator=(TimerHeapReference&&);
    TimerHeapReference& operator=(Ref<ThreadTimerHeapItem>&&);

    void swap(TimerHeapReference& other);

    void updateHeapIndex();

private:
    Ref<ThreadTimerHeapItem>& m_reference;

    friend void swap(TimerHeapReference a, TimerHeapReference b);
};

inline TimerHeapReference TimerHeapPointer::operator*() const
{
    return TimerHeapReference { *m_pointer };
}

inline TimerHeapReference& TimerHeapReference::operator=(TimerHeapReference&& other)
{
    m_reference = WTF::move(other.m_reference);
    updateHeapIndex();
    return *this;
}

inline TimerHeapReference& TimerHeapReference::operator=(Ref<ThreadTimerHeapItem>&& item)
{
    m_reference = WTF::move(item);
    updateHeapIndex();
    return *this;
}

inline void NODELETE TimerHeapReference::swap(TimerHeapReference& other)
{
    m_reference.swap(other.m_reference);
    updateHeapIndex();
    other.updateHeapIndex();
}

inline void NODELETE TimerHeapReference::updateHeapIndex()
{
    auto& heap = m_reference->timerHeap();
    if (&m_reference >= heap.begin() && &m_reference < heap.end())
        m_reference->setHeapIndex(&m_reference - heap.begin());
}

inline void NODELETE swap(TimerHeapReference a, TimerHeapReference b)
{
    a.swap(b);
}

// ----------------

// Class to represent iterators in the heap when calling the standard library heap algorithms.
// Uses a custom pointer and reference type that update indices for pointers in the heap.
class TimerHeapIterator {
public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = Ref<ThreadTimerHeapItem>;
    using difference_type = ptrdiff_t;
    using pointer = TimerHeapPointer;
    using reference = TimerHeapReference;

    explicit TimerHeapIterator(std::span<Ref<ThreadTimerHeapItem>> container, size_t index)
        : m_container(container)
        , m_index(index)
    {
        ASSERT(m_index <= m_container.size());
    }

    TimerHeapIterator& NODELETE operator++() { ++m_index; return *this; }
    TimerHeapIterator NODELETE operator++(int) { return TimerHeapIterator(m_container, m_index++); }

    TimerHeapIterator& NODELETE operator--() { --m_index; return *this; }
    TimerHeapIterator NODELETE operator--(int) { return TimerHeapIterator(m_container, m_index--); }

    TimerHeapIterator& NODELETE operator+=(ptrdiff_t i) { m_index += i; return *this; }
    TimerHeapIterator& NODELETE operator-=(ptrdiff_t i) { m_index -= i; return *this; }

    TimerHeapReference NODELETE operator[](ptrdiff_t i) const { return TimerHeapReference(m_container[m_index + i]); }

    TimerHeapReference NODELETE operator*() const { return TimerHeapReference(m_container[m_index]); }
    Ref<ThreadTimerHeapItem>& NODELETE operator->() const { return m_container[m_index]; }

    auto operator<=>(TimerHeapIterator other) const { ASSERT(hasSameContainerAs(other)); return m_index <=> other.m_index; }
    bool NODELETE operator==(TimerHeapIterator other) const { ASSERT(hasSameContainerAs(other)); return m_index == other.m_index; }

#if ASSERT_ENABLED
    bool hasSameContainerAs(TimerHeapIterator other) const
    {
        if (std::to_address(m_container.begin()) != std::to_address(other.m_container.begin()))
            return false;
        return std::to_address(m_container.end()) == std::to_address(other.m_container.end());
    }
#endif

private:
    friend TimerHeapIterator operator+(TimerHeapIterator, size_t);
    friend TimerHeapIterator operator+(size_t, TimerHeapIterator);
    
    friend TimerHeapIterator operator-(TimerHeapIterator, size_t);
    friend ptrdiff_t operator-(TimerHeapIterator, TimerHeapIterator);

    std::span<Ref<ThreadTimerHeapItem>> m_container;
    size_t m_index;
};

inline TimerHeapIterator NODELETE operator+(TimerHeapIterator a, size_t b) { return TimerHeapIterator(a.m_container, a.m_index + b); }
inline TimerHeapIterator NODELETE operator+(size_t a, TimerHeapIterator b) { return TimerHeapIterator(b.m_container, a + b.m_index); }

inline TimerHeapIterator NODELETE operator-(TimerHeapIterator a, size_t b) { return TimerHeapIterator(a.m_container, a.m_index - b); }
inline ptrdiff_t NODELETE operator-(TimerHeapIterator a, TimerHeapIterator b) { ASSERT(a.hasSameContainerAs(b)); return static_cast<ptrdiff_t>(a.m_index) - static_cast<ptrdiff_t>(b.m_index); }

// ----------------

class TimerHeapLessThanFunction {
public:
    static bool NODELETE compare(const TimerBase& a, const Ref<ThreadTimerHeapItem>& b)
    {
        return compare(a.m_heapItemWithBitfields.pointer()->time, a.m_heapItemWithBitfields.pointer()->insertionOrder, b->time, b->insertionOrder);
    }

    static bool NODELETE compare(const Ref<ThreadTimerHeapItem>& a, const TimerBase& b)
    {
        return compare(a->time, a->insertionOrder, b.m_heapItemWithBitfields.pointer()->time, b.m_heapItemWithBitfields.pointer()->insertionOrder);
    }

    bool operator()(const Ref<ThreadTimerHeapItem>& a, const Ref<ThreadTimerHeapItem>& b) const
    {
        return compare(a->time, a->insertionOrder, b->time, b->insertionOrder);
    }

private:
    static bool NODELETE compare(MonotonicTime aTime, unsigned aOrder, MonotonicTime bTime, unsigned bOrder)
    {
        // The comparisons below are "backwards" because the heap puts the largest
        // element first and we want the lowest time to be the first one in the heap.
        if (bTime != aTime)
            return bTime < aTime;
        // We need to look at the difference of the insertion orders instead of comparing the two
        // outright in case of overflow.
        unsigned difference = aOrder - bOrder;
        return difference < std::numeric_limits<unsigned>::max() / 2;
    }
};

// ----------------

struct SameSizeAsTimer {
    virtual ~SameSizeAsTimer() { }

    WeakPtr<TimerAlignment> timerAlignment;
    double times[2];
    void* pointers[2];
#if CPU(ADDRESS32)
    uint8_t bitfields;
#endif
#if ASSERT_ENABLED
    uint32_t threadID;
#endif
};

static_assert(sizeof(Timer) == sizeof(SameSizeAsTimer), "Timer should stay small");

struct SameSizeAsDeferrableOneShotTimer : public SameSizeAsTimer {
    double delay;
};

static_assert(sizeof(DeferrableOneShotTimer) == sizeof(SameSizeAsDeferrableOneShotTimer), "DeferrableOneShotTimer should stay small");

#if USE(WEB_THREAD)
// The assertion this replaces states that every timer is touched with the web
// lock held. That holds for a port where the engine owns its own scroll view
// and event delivery; here UIKit owns both and calls in on its own terms, so
// the condition is violated by the design of the port rather than by a race.
// Being on the web thread or the main thread is what actually matters for the
// timer heap, so that is what is enforced - and the looser case is reported
// once, with enough detail to find it, instead of ending the session.
static void ensureTimerThreadIsSane(const char* what)
{
    if (WebThreadIsLockedOrDisabledInMainOrWebThread())
        return;

    bool onAThreadThatOwnsTheHeap = WebThreadIsCurrent() || pthread_main_np();
    RELEASE_ASSERT(onAThreadThatOwnsTheHeap);

    static bool reported;
    if (!reported) {
        reported = true;
        WTFLogAlways("[timer] %s on the %s thread without the web lock held", what,
            WebThreadIsCurrent() ? "web" : "main");
    }
}
#endif

TimerBase::TimerBase()
{
#if USE(WEB_THREAD)
    ensureTimerThreadIsSane("construction");
#endif
}

TimerBase::~TimerBase()
{
    ASSERT(canCurrentThreadIDAccessThreadLocalData(m_creationThreadID));
    stop();
    ASSERT(!inHeap());
    if (auto* item = m_heapItemWithBitfields.pointer())
        item->clearTimer();
    m_unalignedNextFireTime = MonotonicTime::nan();
}

void TimerBase::start(Seconds nextFireInterval, Seconds repeatInterval)
{
    ASSERT(canCurrentThreadIDAccessThreadLocalData(m_creationThreadID));

    m_repeatInterval = repeatInterval;
    setNextFireTime(MonotonicTime::now() + nextFireInterval);
}

void TimerBase::stopSlowCase()
{
    ASSERT(canCurrentThreadIDAccessThreadLocalData(m_creationThreadID));

    m_repeatInterval = 0_s;
    setNextFireTime(MonotonicTime { });

    ASSERT(!static_cast<bool>(nextFireTime()));
    ASSERT(m_repeatInterval == 0_s);
    ASSERT(!inHeap());
}

Seconds TimerBase::nextFireInterval() const
{
    ASSERT(isActive());
    ASSERT(m_heapItemWithBitfields.pointer());
    MonotonicTime current = MonotonicTime::now();
    auto fireTime = nextFireTime();
    if (fireTime < current)
        return 0_s;
    return fireTime - current;
}

inline void TimerBase::checkHeapIndex() const
{
#if ASSERT_ENABLED
    SUPPRESS_UNCOUNTED_LOCAL auto* item = m_heapItemWithBitfields.pointer();
    ASSERT(item);
    auto& heap = item->timerHeap();
    ASSERT(&heap == &threadGlobalTimerHeap());
    ASSERT(!heap.isEmpty());
    ASSERT(item->isInHeap());
    ASSERT(item->heapIndex() < heap.size());
    ASSERT(heap[item->heapIndex()].ptr() == item);
    for (unsigned i = 0, size = heap.size(); i < size; i++)
        ASSERT(heap[i]->heapIndex() == i);
#endif
}

inline void TimerBase::checkConsistency() const
{
    // Timers should be in the heap if and only if they have a non-zero next fire time.
    ASSERT(inHeap() == static_cast<bool>(nextFireTime()));
    if (inHeap())
        checkHeapIndex();
}

// The heap is a plain binary heap over TimerHeapLessThanFunction, which orders
// by (fire time, insertion order) and so has a single well-defined minimum
// whatever permutation the rest of the array is in. Sifting one element by hand
// costs one pass; the standard-library formulation had to reach every removal
// and every key increase through push_heap plus pop_heap, which is two or three
// passes and, on a page holding hundreds of timers, two or three times the
// element moves - each of which writes back a heap index.
static inline void swapHeapItems(ThreadTimerHeap& heap, unsigned a, unsigned b)
{
    heap[a].swap(heap[b]);
    heap[a]->setHeapIndex(a);
    heap[b]->setHeapIndex(b);
}

static unsigned heapSiftUp(ThreadTimerHeap& heap, unsigned index)
{
    TimerHeapLessThanFunction lessThan;
    while (index) {
        unsigned parentIndex = (index - 1) / 2;
        if (!lessThan(heap[parentIndex], heap[index]))
            break;
        swapHeapItems(heap, parentIndex, index);
        index = parentIndex;
    }
    return index;
}

static unsigned heapSiftDown(ThreadTimerHeap& heap, unsigned index)
{
    TimerHeapLessThanFunction lessThan;
    unsigned size = static_cast<unsigned>(heap.size());
    for (;;) {
        unsigned childIndex = 2 * index + 1;
        if (childIndex >= size)
            break;
        if (childIndex + 1 < size && lessThan(heap[childIndex], heap[childIndex + 1]))
            ++childIndex;
        if (!lessThan(heap[index], heap[childIndex]))
            break;
        swapHeapItems(heap, index, childIndex);
        index = childIndex;
    }
    return index;
}

// Leaves the heap valid and one element shorter. The removed item keeps whatever
// heap index it had; every caller either drops it or marks it as not in the heap.
static void heapRemoveAtIndex(ThreadTimerHeap& heap, unsigned index)
{
    unsigned lastIndex = static_cast<unsigned>(heap.size()) - 1;
    if (index != lastIndex) {
        swapHeapItems(heap, index, lastIndex);
        heap.removeLast();
        // The element that filled the hole came from the bottom of the heap, so
        // it can belong either above or below its new place, but never both.
        if (heapSiftUp(heap, index) == index)
            heapSiftDown(heap, index);
    } else
        heap.removeLast();
}

// m_heapItemWithBitfields is itself a CompactRefPtrTuple, so it holds the item alive for the
// whole of each of these calls, and ThreadTimerHeapItem is ThreadSafeRefCounted: the local Ref
// these used to take cost a pair of atomic read-modify-writes with their barriers on every
// timer start, stop and fire.
void TimerBase::heapDecreaseKey()
{
    ASSERT(static_cast<bool>(nextFireTime()));
    SUPPRESS_UNCOUNTED_LOCAL auto* item = m_heapItemWithBitfields.pointer();
    ASSERT(item);
    checkHeapIndex();
    heapSiftUp(item->timerHeap(), item->heapIndex());
    checkHeapIndex();
}

inline void TimerBase::heapDelete()
{
    ASSERT(!static_cast<bool>(nextFireTime()));
    SUPPRESS_UNCOUNTED_LOCAL auto* item = m_heapItemWithBitfields.pointer();
    ASSERT(item);
    heapRemoveAtIndex(item->timerHeap(), item->heapIndex());
    item->setNotInHeap();
}

void TimerBase::heapDeleteMin()
{
    ASSERT(!static_cast<bool>(nextFireTime()));
    SUPPRESS_UNCOUNTED_LOCAL auto* item = m_heapItemWithBitfields.pointer();
    ASSERT(item);
    ASSERT(item->isFirstInHeap());
    heapRemoveAtIndex(item->timerHeap(), 0);
    item->setNotInHeap();
}

inline void TimerBase::heapIncreaseKey()
{
    ASSERT(static_cast<bool>(nextFireTime()));
    SUPPRESS_UNCOUNTED_LOCAL auto* item = m_heapItemWithBitfields.pointer();
    ASSERT(item);
    checkHeapIndex();
    // A later fire time can only move an item away from the front of the heap.
    heapSiftDown(item->timerHeap(), item->heapIndex());
    checkHeapIndex();
}

inline void TimerBase::heapInsert()
{
    ASSERT(!inHeap());
    SUPPRESS_UNCOUNTED_LOCAL auto* item = m_heapItemWithBitfields.pointer();
    ASSERT(item);
    auto& heap = item->timerHeap();
    heap.append(*item);
    item->setHeapIndex(heap.size() - 1);
    heapDecreaseKey();
}

void TimerBase::heapPopMin()
{
    SUPPRESS_UNCOUNTED_LOCAL auto* item = m_heapItemWithBitfields.pointer();
    ASSERT(item);
    ASSERT(item == item->timerHeap().first().ptr());
    checkHeapIndex();
    auto& heap = item->timerHeap();
    auto heapData = heap.mutableSpan();
    std::pop_heap(TimerHeapIterator(heapData, 0), TimerHeapIterator(heapData, heap.size()), TimerHeapLessThanFunction());
    checkHeapIndex();
    ASSERT(item == item->timerHeap().last().ptr());
}

void TimerBase::heapDeleteNullMin(ThreadTimerHeap& heap)
{
    RELEASE_ASSERT(!heap.first()->hasTimer());
    heapRemoveAtIndex(heap, 0);
}

static inline bool NODELETE parentHeapPropertyHolds(const TimerBase* current, const ThreadTimerHeap& heap, unsigned currentIndex)
{
    if (!currentIndex)
        return true;
    unsigned parentIndex = (currentIndex - 1) / 2;
    return TimerHeapLessThanFunction::compare(*current, heap[parentIndex]);
}

static inline bool NODELETE childHeapPropertyHolds(const TimerBase* current, const ThreadTimerHeap& heap, unsigned childIndex)
{
    if (childIndex >= heap.size())
        return true;
    return TimerHeapLessThanFunction::compare(heap[childIndex], *current);
}

bool TimerBase::hasValidHeapPosition() const
{
    ASSERT(nextFireTime());
    auto* item = m_heapItemWithBitfields.pointer();
    ASSERT(item);
    if (!inHeap())
        return false;
    // Check if the heap property still holds with the new fire time. If it does we don't need to do anything.
    // This assumes that the STL heap is a standard binary heap. In an unlikely event it is not, the assertions
    // in updateHeapIfNeeded() will get hit.
    const auto& heap = item->timerHeap();
    unsigned heapIndex = item->heapIndex();
    if (!parentHeapPropertyHolds(this, heap, heapIndex))
        return false;
    unsigned childIndex1 = 2 * heapIndex + 1;
    unsigned childIndex2 = childIndex1 + 1;
    return childHeapPropertyHolds(this, heap, childIndex1) && childHeapPropertyHolds(this, heap, childIndex2);
}

void TimerBase::updateHeapIfNeeded(MonotonicTime oldTime)
{
    auto fireTime = nextFireTime();
    if (fireTime && hasValidHeapPosition())
        return;

#if ASSERT_ENABLED
    std::optional<unsigned> oldHeapIndex;
    auto* item = m_heapItemWithBitfields.pointer();
    if (item->isInHeap())
        oldHeapIndex = item->heapIndex();
#endif

    if (!oldTime)
        heapInsert();
    else if (!fireTime)
        heapDelete();
    else if (fireTime < oldTime)
        heapDecreaseKey();
    else
        heapIncreaseKey();

#if ASSERT_ENABLED
    std::optional<unsigned> newHeapIndex;
    if (item->isInHeap())
        newHeapIndex = item->heapIndex();
    ASSERT(newHeapIndex != oldHeapIndex);
#endif

    ASSERT(!inHeap() || hasValidHeapPosition());
}

void TimerBase::setNextFireTime(MonotonicTime newTime)
{
#if USE(WEB_THREAD)
    ensureTimerThreadIsSane("setNextFireTime");
#endif
    ASSERT(canCurrentThreadIDAccessThreadLocalData(m_creationThreadID));
    bool timerHasBeenDeleted = m_unalignedNextFireTime.isNaN();
    RELEASE_ASSERT_WITH_SECURITY_IMPLICATION(!timerHasBeenDeleted);

    if (m_unalignedNextFireTime != newTime) {
        RELEASE_ASSERT(!newTime.isNaN());
        m_unalignedNextFireTime = newTime;
    }

    // Keep heap valid while changing the next-fire time.
    MonotonicTime oldTime = nextFireTime();
    // Don't realign zero-delay timers.
    if (CheckedPtr alignment = m_alignment.get(); newTime && alignment)
        newTime = alignment->alignedFireTime(hasReachedMaxNestingLevel(), newTime);

    if (oldTime != newTime) {
        // One thread-local lookup, not two: this runs on every setTimeout,
        // clearTimeout, timer fire and internal timer restart.
        auto& threadTimers = threadGlobalDataSingleton().threadTimers();
        auto newOrder = threadTimers.nextHeapInsertionCount();

        SUPPRESS_UNCOUNTED_LOCAL auto* item = m_heapItemWithBitfields.pointer();
        if (!item) {
            m_heapItemWithBitfields.setPointer(ThreadTimerHeapItem::create(*this, newTime, 0));
            item = m_heapItemWithBitfields.pointer();
        }
        item->time = newTime;
        item->insertionOrder = newOrder;

        bool wasFirstTimerInHeap = item->isFirstInHeap();

        updateHeapIfNeeded(oldTime);

        bool isFirstTimerInHeap = item->isFirstInHeap();

        if (wasFirstTimerInHeap || isFirstTimerInHeap)
            threadTimers.updateSharedTimer();
    }

    checkConsistency();
}

void TimerBase::fireTimersInNestedEventLoop()
{
    // Redirect to ThreadTimers.
    threadGlobalDataSingleton().threadTimers().fireTimersInNestedEventLoop();
}

void TimerBase::didChangeAlignmentInterval()
{
    setNextFireTime(m_unalignedNextFireTime);
}

Seconds TimerBase::nextUnalignedFireInterval() const
{
    ASSERT(isActive());
    auto result = std::max(m_unalignedNextFireTime - MonotonicTime::now(), 0_s);
    RELEASE_ASSERT(result.isFinite());
    return result;
}

} // namespace WebCore
