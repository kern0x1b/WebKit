/*
 * Copyright (C) 2012-2023 Apple Inc. All rights reserved.
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
#include "SlotVisitor.h"

#include "CellContainerInlines.h"
#include "ConservativeRoots.h"
#include "GCSegmentedArrayInlines.h"
#include "HeapAnalyzer.h"
#include "HeapCellInlines.h"
#include "HeapProfiler.h"
#include "IntegrityInlines.h"
#include "JSArray.h"
#include "JSCellInlines.h"
#include "JSString.h"
#include "MarkingConstraintSolver.h"
#include "SlotVisitorInlines.h"
#include "StopIfNecessaryTimer.h"
#include "VM.h"
#include <wtf/ListDump.h>
#include <wtf/Lock.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(SlotVisitor);

#if ENABLE(GC_VALIDATION)
static void validate(JSCell* cell)
{
    RELEASE_ASSERT(cell);

    if (!cell->structure()) {
        dataLogF("cell at %p has a null structure\n" , cell);
        CRASH();
    }

    // Both the cell's structure, and the cell's structure's structure should be the Structure Structure.
    // I hate this sentence.
    if (cell->structure()->structure()->JSCell::classInfo() != cell->structure()->JSCell::classInfo()) {
        const char* parentClassName = 0;
        const char* ourClassName = 0;
        if (cell->structure()->structure() && cell->structure()->structure()->JSCell::classInfo())
            parentClassName = cell->structure()->structure()->JSCell::classInfo()->className;
        if (cell->structure()->JSCell::classInfo())
            ourClassName = cell->structure()->JSCell::classInfo()->className;
        dataLogF("parent structure (%p <%s>) of cell at %p doesn't match cell's structure (%p <%s>)\n",
            cell->structure()->structure(), parentClassName, cell, cell->structure(), ourClassName);
        CRASH();
    }

    // Make sure we can walk the ClassInfo chain
    const ClassInfo* info = cell->classInfo();
    do { } while ((info = info->parentClass));
}
#endif

SlotVisitor::SlotVisitor(JSC::Heap& heap, CString codeName)
    : Base(heap, codeName, heap.m_opaqueRoots)
    , m_markingVersion(MarkedSpace::initialVersion)
#if ASSERT_ENABLED
    , m_isCheckingForDefaultMarkViolation(false)
#endif
{
}

SlotVisitor::~SlotVisitor()
{
    clearMarkStacks();
}

void SlotVisitor::didStartMarking()
{
    auto scope = heap()->collectionScope();
    if (scope) {
        switch (*scope) {
        case CollectionScope::Eden:
            reset();
            break;
        case CollectionScope::Full:
            m_extraMemorySize = 0;
            break;
        }
    }

    if (HeapProfiler* heapProfiler = vm().heapProfiler())
        m_heapAnalyzer = heapProfiler->activeHeapAnalyzer();

    m_markingVersion = heap()->objectSpace().markingVersion();
#if defined(WEBKIT_IOS6)
    m_needsMarkingFence = m_heap.m_hasParallelMarkers;
#endif
}

void SlotVisitor::reset()
{
    AbstractSlotVisitor::reset();
    m_bytesVisited = 0;
    m_heapAnalyzer = nullptr;
    RELEASE_ASSERT(!m_currentCell);
}

void SlotVisitor::clearMarkStacks()
{
    forEachMarkStack(
        [&] (MarkStackArray& stack) -> IterationStatus {
            stack.clear();
            return IterationStatus::Continue;
        });
}

void SlotVisitor::append(const ConservativeRoots& conservativeRoots)
{
    HeapCell** roots = conservativeRoots.roots();
    size_t size = conservativeRoots.size();
    for (size_t i = 0; i < size; ++i)
        appendJSCellOrAuxiliary(roots[i]);
}

void SlotVisitor::appendJSCellOrAuxiliary(HeapCell* heapCell)
{
    if (!heapCell)
        return;
    
    ASSERT(!m_isCheckingForDefaultMarkViolation);
    
    auto validateCell = [&] (JSCell* jsCell) {
        StructureID structureID = jsCell->structureID();
        
        auto die = [&] (const char* text) {
            WTF::dataFile().atomically(
                [&] (PrintStream& out) {
                    out.print(text);
                    out.print("GC type: ", heap()->collectionScope(), "\n");
                    out.print("Object at: ", RawPointer(jsCell), "\n");
#if USE(JSVALUE64)
                    out.print("Structure ID: ", structureID.bits(), " (", RawPointer(structureID.decode()), ")\n");
#else
                    out.print("Structure: ", RawPointer(structureID.decode()), "\n");
#endif
                    out.print("Object contents:");
                    for (unsigned i = 0; i < 2; ++i)
                        out.print(" ", format("0x%016llx", std::bit_cast<uint64_t*>(jsCell)[i]));
                    out.print("\n");
                    CellContainer container = jsCell->cellContainer();
                    out.print("Is marked: ", container.isMarked(jsCell), "\n");
                    out.print("Is newly allocated: ", container.isNewlyAllocated(jsCell), "\n");
                    if (container.isMarkedBlock()) {
                        MarkedBlock& block = container.markedBlock();
                        out.print("Block: ", RawPointer(&block), "\n");
                        block.handle().dumpState(out);
                        out.print("\n");
                        out.print("Is marked raw: ", block.isMarkedRaw(jsCell), "\n");
                        out.print("Marking version: ", block.markingVersion(), "\n");
                        out.print("Heap marking version: ", heap()->objectSpace().markingVersion(), "\n");
                        out.print("Is newly allocated raw: ", block.isNewlyAllocated(jsCell), "\n");
                        out.print("Newly allocated version: ", block.newlyAllocatedVersion(), "\n");
                        out.print("Heap newly allocated version: ", heap()->objectSpace().newlyAllocatedVersion(), "\n");
                    }
                    UNREACHABLE_FOR_PLATFORM();
                });
        };
        
        // It's not OK for the structure to be null at any GC scan point. We must not GC while
        // an object is not fully initialized.
        if (!structureID)
            die("GC scan found corrupt object: structureID is zero!\n");
        
        // It's not OK for the structure to be nuked at any GC scan point.
        if (structureID.isNuked())
            die("GC scan found object in bad state: structureID is nuked!\n");

        // This detects the worst of the badness.
        Integrity::auditStructureID(structureID);
    };

    // In debug mode, we validate before marking since this makes it clearer what the problem
    // was. It's also slower, so we don't do it normally.
    if (ASSERT_ENABLED && isJSCellKind(heapCell->cellKind()))
        validateCell(static_cast<JSCell*>(heapCell));
    
    if (Heap::testAndSetMarked(m_markingVersion, heapCell))
        return;
    
    switch (heapCell->cellKind()) {
    case HeapCell::JSCell:
    case HeapCell::JSCellWithIndexingHeader: {
        // We have ample budget to perform validation here.
    
        JSCell* jsCell = static_cast<JSCell*>(heapCell);
        validateCell(jsCell);
        Integrity::auditCell(vm(), jsCell);
        
        jsCell->setCellState(CellState::PossiblyGrey);

        appendToMarkStack(jsCell);
        return;
    }
        
    case HeapCell::Auxiliary: {
        noteLiveAuxiliaryCell(heapCell);
        return;
    } }
}

void SlotVisitor::appendSlow(JSCell* cell, Dependency dependency)
{
    if (m_heapAnalyzer) [[unlikely]]
        m_heapAnalyzer->analyzeEdge(m_currentCell, cell, rootMarkReason());

    appendHiddenSlowImpl(cell, dependency);
}

void SlotVisitor::appendHiddenSlow(JSCell* cell, Dependency dependency)
{
    appendHiddenSlowImpl(cell, dependency);
}

ALWAYS_INLINE void SlotVisitor::appendHiddenSlowImpl(JSCell* cell, Dependency dependency)
{
    ASSERT(!m_isCheckingForDefaultMarkViolation);

#if ENABLE(GC_VALIDATION)
    validate(cell);
#endif
    
    if (cell->isPreciseAllocation())
        setMarkedAndAppendToMarkStack(cell->preciseAllocation(), cell, dependency);
    else
        setMarkedAndAppendToMarkStack(cell->markedBlock(), cell, dependency);
}

template<typename ContainerType>
ALWAYS_INLINE void SlotVisitor::setMarkedAndAppendToMarkStack(ContainerType& container, JSCell* cell, Dependency dependency)
{
    if (container.testAndSetMarked(cell, dependency))
        return;

    ASSERT(cell->structure());

    // Indicate that the object is grey and that:
    // In case of concurrent GC: it's the first time it is grey in this GC cycle.
    // In case of eden collection: it's a new object that became grey rather than an old remembered object.
    cell->setCellState(CellState::PossiblyGrey);

    appendToMarkStack(container, cell);
}

#if defined(WEBKIT_IOS6)
void SlotVisitor::setMarkedAndAppendToMarkStack(MarkedBlock& block, JSCell* cell, Dependency dependency)
{
    bool alreadyMarked = (!m_needsMarkingFence && webkitIOS6GCUncontendedMarkEnabled())
        ? block.testAndSetMarkedUncontended(cell, dependency)
        : block.testAndSetMarked(cell, dependency);
    if (alreadyMarked)
        return;

    ASSERT(cell->structure());

    cell->setCellState(CellState::PossiblyGrey);

    appendToMarkStack(block, cell);
}
#endif

void SlotVisitor::appendToMarkStack(JSCell* cell)
{
    if (cell->isPreciseAllocation())
        appendToMarkStack(cell->preciseAllocation(), cell);
    else
        appendToMarkStack(cell->markedBlock(), cell);
}

template<typename ContainerType>
ALWAYS_INLINE void SlotVisitor::appendToMarkStack(ContainerType& container, JSCell* cell)
{
    ASSERT(m_heap.isMarked(cell));
#if CPU(X86_64)
    if (Options::dumpZappedCellCrashData()) [[unlikely]] {
        if (cell->isZapped()) [[unlikely]]
            reportZappedCellAndCrash(cell);
    }
#endif
    ASSERT(!cell->isZapped());

    container.noteMarked();
    
    m_visitCount++;
    m_bytesVisited += container.cellSize();

    m_collectorStack.append(cell);
}

void SlotVisitor::markAuxiliary(const void* base)
{
    HeapCell* cell = std::bit_cast<HeapCell*>(base);
    
    ASSERT(cell->heap() == heap());
    
    if (Heap::testAndSetMarked(m_markingVersion, cell))
        return;
    
    noteLiveAuxiliaryCell(cell);
}

void SlotVisitor::noteLiveAuxiliaryCell(HeapCell* cell)
{
    // We get here once per GC under these circumstances:
    //
    // Eden collection: if the cell was allocated since the last collection and is live somehow.
    //
    // Full collection: if the cell is live somehow.
    
    CellContainer container = cell->cellContainer();
    
    container.assertValidCell(vm(), cell);
    container.noteMarked();
    
    m_visitCount++;

    size_t cellSize = container.cellSize();
    m_bytesVisited += cellSize;
    m_nonCellVisitCount += cellSize;
}

class SetCurrentCellScope {
public:
    SetCurrentCellScope(SlotVisitor& visitor, const JSCell* cell)
        : m_visitor(visitor)
    {
        ASSERT(!m_visitor.m_currentCell);
        m_visitor.m_currentCell = const_cast<JSCell*>(cell);
    }

    ~SetCurrentCellScope()
    {
        ASSERT(m_visitor.m_currentCell);
        m_visitor.m_currentCell = nullptr;
    }

private:
    SlotVisitor& m_visitor;
};

ALWAYS_INLINE void SlotVisitor::visitChildren(const JSCell* cell)
{
    ASSERT(m_heap.isMarked(cell));
    
    SetCurrentCellScope currentCellScope(*this, cell);
    
    if (false) {
        dataLog("Visiting ", RawPointer(cell));
        if (!m_isFirstVisit)
            dataLog(" (subsequent)");
        dataLog("\n");
    }
    
    // Funny story: it's possible for the object to be black already, if we barrier the object at
    // about the same time that it's marked. That's fine. It's a gnarly and super-rare race. It's
    // not clear to me that it would be correct or profitable to bail here if the object is already
    // black.
    
    cell->setCellState(CellState::PossiblyBlack);

#if defined(WEBKIT_IOS6)
    // This fence pairs with the storeLoadFence in Heap::writeBarrierSlowPath: it stops us reading a
    // stale field while a barrier on another thread reads a stale, not-yet-black cell state. On this
    // CPU Options::useConcurrentGC() is forced off, so the world is stopped for the whole mark phase
    // and the only other threads that can execute a barrier against a cell we are scanning are the
    // helper markers. With no helper markers there is no second party, and this is a full dmb on
    // armv7 - paid once per marked cell.
    if (m_needsMarkingFence) [[likely]]
        WTF::storeLoadFence();
#else
    WTF::storeLoadFence();
#endif

    switch (cell->type()) {
    case StringType:
        JSString::visitChildren(const_cast<JSCell*>(cell), *this);
        break;
        
    case FinalObjectType:
        JSFinalObject::visitChildren(const_cast<JSCell*>(cell), *this);
        break;

    case ArrayType:
        JSArray::visitChildren(const_cast<JSCell*>(cell), *this);
        break;
        
    default:
        // FIXME: This could be so much better.
        // https://bugs.webkit.org/show_bug.cgi?id=162462
#if CPU(X86_64)
        if (Options::dumpZappedCellCrashData()) [[unlikely]] {
            Structure* structure = cell->structure();
            if (structure) [[likely]] {
                const MethodTable* methodTable = &structure->classInfoForCells()->methodTable;
                methodTable->visitChildren(const_cast<JSCell*>(cell), *this);
                break;
            }
            reportZappedCellAndCrash(const_cast<JSCell*>(cell));
        }
#endif
        cell->methodTable()->visitChildren(const_cast<JSCell*>(cell), *this);
        break;
    }

    if (m_heapAnalyzer) [[unlikely]] {
        if (m_isFirstVisit)
            m_heapAnalyzer->analyzeNode(const_cast<JSCell*>(cell));
    }
}

void SlotVisitor::visitAsConstraint(const JSCell* cell)
{
    m_isFirstVisit = false;
    visitChildren(cell);
}

inline void SlotVisitor::propagateExternalMemoryVisitedIfNecessary()
{
    if (m_isFirstVisit) {
        if (m_extraMemorySize.hasOverflowed())
            heap()->reportExtraMemoryVisited(std::numeric_limits<size_t>::max());
        else if (m_extraMemorySize)
            heap()->reportExtraMemoryVisited(m_extraMemorySize);
        m_extraMemorySize = 0;
    }
}

void SlotVisitor::donateKnownParallel(MarkStackArray& from, MarkStackArray& to)
{
    // NOTE: Because we re-try often, we can afford to be conservative, and
    // assume that donating is not profitable.

    // Avoid locking when a thread reaches a dead end in the object graph.
    if (from.size() < 2)
        return;

    // If there's already some shared work queued up, be conservative and assume
    // that donating more is not profitable.
    if (to.size())
        return;

    // If we're contending on the lock, be conservative and assume that another
    // thread is already donating.
    if (!m_heap.m_markingMutex.tryLock())
        return;
    Locker locker { AdoptLock, m_heap.m_markingMutex };

    // Otherwise, assume that a thread will go idle soon, and donate.
    from.donateSomeCellsTo(to);

    m_heap.m_markingConditionVariable.notifyAll();
}

void SlotVisitor::donateKnownParallel()
{
    // With no helper markers there is nobody on the other end of the shared stacks, so every step
    // below - two stack size loads, a tryLock on the shared m_markingMutex, and a broadcast on the
    // marking condition variable - is pure overhead. drain() calls us directly on every rebalance
    // interval, so this check has to live here and not only in donate().
    if (!m_heap.m_hasParallelMarkers)
        return;

    forEachMarkStack(
        [&] (MarkStackArray& stack) -> IterationStatus {
            donateKnownParallel(stack, correspondingGlobalStack(stack));
            return IterationStatus::Continue;
        });
}

void SlotVisitor::updateMutatorIsStopped(const AbstractLocker&)
{
    m_mutatorIsStopped = (m_heap.worldIsStopped() & m_canOptimizeForStoppedMutator);
}

void SlotVisitor::updateMutatorIsStopped()
{
    if (mutatorIsStoppedIsUpToDate())
        return;
    updateMutatorIsStopped(Locker { m_rightToRun });
}

bool SlotVisitor::hasAcknowledgedThatTheMutatorIsResumed() const
{
    return !m_mutatorIsStopped;
}

bool SlotVisitor::mutatorIsStoppedIsUpToDate() const
{
    return m_mutatorIsStopped == (m_heap.worldIsStopped() & m_canOptimizeForStoppedMutator);
}

void SlotVisitor::optimizeForStoppedMutator()
{
    m_canOptimizeForStoppedMutator = true;
}

#if defined(WEBKIT_IOS6)
static unsigned NODELETE envUnsigned(const char* name, unsigned defaultValue)
{
    const char* text = getenv(name);
    if (!text || !text[0])
        return defaultValue;
    char* end = nullptr;
    long value = strtol(text, &end, 10);
    if (end == text || value < 0)
        return defaultValue;
    return static_cast<unsigned>(value);
}

static constexpr unsigned ios6MaxMarkPipelineDepth = 16;

static unsigned webkitIOS6GCMarkPipelineDepth()
{
    static const unsigned depth = std::min(envUnsigned("WEBKIT_IOS6_GC_MARK_PIPELINE_DEPTH", 0), ios6MaxMarkPipelineDepth);
    return depth;
}

template<typename ShouldStop, typename OnVisit>
ALWAYS_INLINE static void drainMarkStackPipelined(MarkStackArray& stack, unsigned depth, const ShouldStop& shouldStop, const OnVisit& onVisit)
{
    const JSCell* queue[ios6MaxMarkPipelineDepth];
    unsigned head = 0;
    unsigned count = 0;

    auto pushOne = [&] ALWAYS_INLINE_LAMBDA -> bool {
        if (count == depth || shouldStop() || !stack.canRemoveLast())
            return false;
        queue[(head + count) % ios6MaxMarkPipelineDepth] = stack.popAndPrefetch();
        ++count;
        return true;
    };

    while (pushOne()) { }

    while (count) {
        if (count > 1 && webkitIOS6GCStructurePrefetchEnabled()) [[likely]] {
            const JSCell* upcoming = queue[(head + 1) % ios6MaxMarkPipelineDepth];
            __builtin_prefetch(upcoming->structureID().tryDecode());
        }
        const JSCell* cell = queue[head];
        head = (head + 1) % ios6MaxMarkPipelineDepth;
        --count;
        pushOne();
        onVisit(cell);
    }
}
#endif

NEVER_INLINE void SlotVisitor::drain(MonotonicTime timeout)
{
    if (!m_isInParallelMode) {
        dataLog("FATAL: attempting to drain when not in parallel mode.\n");
        RELEASE_ASSERT_NOT_REACHED();
    }
    
    Locker locker { m_rightToRun };

    // Both of these are fixed for the whole drain. Reading the option out of its global on every
    // rebalance interval, and calling into the out-of-line hasElapsed() when the caller passed the
    // infinite timeout that a stop-the-world collector always passes, are both pure overhead.
    const unsigned scansBetweenRebalance = Options::minimumNumberOfScansBetweenRebalance();
    const bool neverTimesOut = timeout == MonotonicTime::infinity();

    while (neverTimesOut || !hasElapsed(timeout)) {
        updateMutatorIsStopped(locker);
        IterationStatus status = forEachMarkStack(
            [&] (MarkStackArray& stack) -> IterationStatus {
                if (stack.isEmpty())
                    return IterationStatus::Continue;

                stack.refill();

                m_isFirstVisit = (&stack == &m_collectorStack);

                // The highest cost of GC marking is a cache miss when reading a cell, coming from the GC marker stack,
                // because each cell would be likely placed in a random place. We perform software prefetching onto
                // one next cell while accessing the current cell to make memory fetching in flight while handling
                // the current cell.
                unsigned countdown = scansBetweenRebalance;
#if defined(WEBKIT_IOS6)
                if (unsigned pipelineDepth = webkitIOS6GCMarkPipelineDepth()) {
                    drainMarkStackPipelined(stack, pipelineDepth,
                        [&] ALWAYS_INLINE_LAMBDA -> bool {
                            if (!countdown)
                                return true;
                            --countdown;
                            return false;
                        },
                        [&] (const JSCell* cell) ALWAYS_INLINE_LAMBDA {
                            visitChildren(cell);
                        });
                    return IterationStatus::Done;
                }
#endif
                auto popAndPrefetch = [&] ALWAYS_INLINE_LAMBDA -> const JSCell* {
                    if (!countdown || !stack.canRemoveLast())
                        return nullptr;
                    --countdown;
                    return stack.popAndPrefetch();
                };
                for (const JSCell* next = popAndPrefetch(); next;) {
                    const JSCell* cell = next;
#if defined(WEBKIT_IOS6)
                    if (webkitIOS6GCStructurePrefetchEnabled()) [[likely]]
                        __builtin_prefetch(cell->structureID().tryDecode());
#endif
                    next = popAndPrefetch();
                    visitChildren(cell);
                }
                return IterationStatus::Done;
            });
        propagateExternalMemoryVisitedIfNecessary();
        if (status == IterationStatus::Continue)
            break;

        m_rightToRun.safepoint();
        donateKnownParallel();
    }
}

size_t SlotVisitor::performIncrementOfDraining(size_t bytesRequested)
{
    RELEASE_ASSERT(m_isInParallelMode);

    size_t cellsRequested = bytesRequested / MarkedBlock::atomSize;
    {
        Locker locker { m_heap.m_markingMutex };
        forEachMarkStack(
            [&] (MarkStackArray& stack) -> IterationStatus {
                cellsRequested -= correspondingGlobalStack(stack).transferTo(stack, cellsRequested);
                return cellsRequested ? IterationStatus::Continue : IterationStatus::Done;
            });
    }

    size_t cellBytesVisited = 0;
    m_nonCellVisitCount = 0;

    auto bytesVisited = [&] () -> size_t {
        return cellBytesVisited + m_nonCellVisitCount;
    };

    auto isDone = [&] () -> bool {
        return bytesVisited() >= bytesRequested;
    };
    
    const unsigned scansBetweenRebalance = Options::minimumNumberOfScansBetweenRebalance();

    {
        Locker locker { m_rightToRun };

        while (!isDone()) {
            updateMutatorIsStopped(locker);
            IterationStatus status = forEachMarkStack(
                [&] (MarkStackArray& stack) -> IterationStatus {
                    if (stack.isEmpty() || isDone())
                        return IterationStatus::Continue;

                    stack.refill();

                    m_isFirstVisit = (&stack == &m_collectorStack);

                    unsigned countdown = scansBetweenRebalance;
#if defined(WEBKIT_IOS6)
                    if (unsigned pipelineDepth = webkitIOS6GCMarkPipelineDepth()) {
                        drainMarkStackPipelined(stack, pipelineDepth,
                            [&] ALWAYS_INLINE_LAMBDA -> bool {
                                if (!countdown || isDone())
                                    return true;
                                --countdown;
                                return false;
                            },
                            [&] (const JSCell* cell) ALWAYS_INLINE_LAMBDA {
                                cellBytesVisited += cell->cellSize();
                                visitChildren(cell);
                            });
                        return IterationStatus::Done;
                    }
#endif
                    auto popAndPrefetch = [&] ALWAYS_INLINE_LAMBDA -> const JSCell* {
                        if (!countdown || !stack.canRemoveLast() || isDone())
                            return nullptr;
                        --countdown;
                        return stack.popAndPrefetch();
                    };
                    for (const JSCell* next = popAndPrefetch(); next;) {
                        const JSCell* cell = next;
#if defined(WEBKIT_IOS6)
                        if (webkitIOS6GCStructurePrefetchEnabled()) [[likely]]
                            __builtin_prefetch(cell->structureID().tryDecode());
#endif
                        next = popAndPrefetch();
                        cellBytesVisited += cell->cellSize();
                        visitChildren(cell);
                    }
                    return IterationStatus::Done;
                });
            propagateExternalMemoryVisitedIfNecessary();
            if (status == IterationStatus::Continue)
                break;
            m_rightToRun.safepoint();
            donateKnownParallel();
        }
    }

    donateAll();

    return bytesVisited();
}

bool SlotVisitor::didReachTermination()
{
    Locker locker { m_heap.m_markingMutex };
    return didReachTermination(locker);
}

bool SlotVisitor::didReachTermination(const AbstractLocker& locker)
{
    return !m_heap.m_numberOfActiveParallelMarkers
        && !hasWork(locker);
}

bool SlotVisitor::hasWork(const AbstractLocker&)
{
    return !isEmpty()
        || !m_heap.m_sharedCollectorMarkStack->isEmpty()
        || !m_heap.m_sharedMutatorMarkStack->isEmpty();
}

NEVER_INLINE SlotVisitor::SharedDrainResult SlotVisitor::drainFromShared(SharedDrainMode sharedDrainMode, MonotonicTime timeout)
{
    ASSERT(m_isInParallelMode);
    
    ASSERT(Options::numberOfGCMarkers());

    bool isActive = false;
    while (true) {
        RefPtr<SharedTask<void(SlotVisitor&)>> bonusTask;
        
        {
            Locker locker { m_heap.m_markingMutex };
            if (isActive)
                m_heap.m_numberOfActiveParallelMarkers--;
            m_heap.m_numberOfWaitingParallelMarkers++;
            
            if (sharedDrainMode == MainDrain) {
                while (true) {
                    if (hasElapsed(timeout))
                        return SharedDrainResult::TimedOut;

                    if (didReachTermination(locker)) {
                        m_heap.m_markingConditionVariable.notifyAll();
                        return SharedDrainResult::Done;
                    }
                    
                    if (hasWork(locker))
                        break;

                    m_heap.m_markingConditionVariable.waitUntil(m_heap.m_markingMutex, timeout);
                }
            } else {
                ASSERT(sharedDrainMode == HelperDrain);

                if (hasElapsed(timeout))
                    return SharedDrainResult::TimedOut;
                
                if (didReachTermination(locker)) {
                    m_heap.m_markingConditionVariable.notifyAll();
                    
                    // If we're in concurrent mode, then we know that the mutator will eventually do
                    // the right thing because:
                    // - It's possible that the collector has the conn. In that case, the collector will
                    //   wake up from the notification above. This will happen if the app released heap
                    //   access. Native apps can spend a lot of time with heap access released.
                    // - It's possible that the mutator will allocate soon. Then it will check if we
                    //   reached termination. This is the most likely outcome in programs that allocate
                    //   a lot.
                    // - WebCore never releases access. But WebCore has a runloop. The runloop will check
                    //   if we reached termination.
                    // So, this tells the runloop that it's got things to do.
#if defined(WEBKIT_IOS6)
                    if (!m_heap.worldIsStopped())
                        m_heap.m_stopIfNecessaryTimer->scheduleSoon();
#else
                    m_heap.m_stopIfNecessaryTimer->scheduleSoon();
#endif
                }

                auto isReady = [&] () -> bool {
                    return hasWork(locker)
                        || m_heap.m_bonusVisitorTask
                        || m_heap.m_parallelMarkersShouldExit;
                };

                m_heap.m_markingConditionVariable.waitUntil(m_heap.m_markingMutex, timeout, isReady);
                
                if (!hasWork(locker)
                    && m_heap.m_bonusVisitorTask)
                    bonusTask = m_heap.m_bonusVisitorTask;
                
                if (m_heap.m_parallelMarkersShouldExit)
                    return SharedDrainResult::Done;
            }
            
            if (!bonusTask && isEmpty()) {
                forEachMarkStack(
                    [&] (MarkStackArray& stack) -> IterationStatus {
                        stack.stealSomeCellsFrom(
                            correspondingGlobalStack(stack),
                            m_heap.m_numberOfWaitingParallelMarkers);
                        return IterationStatus::Continue;
                    });
            }

            m_heap.m_numberOfActiveParallelMarkers++;
            m_heap.m_numberOfWaitingParallelMarkers--;
        }
        
        if (bonusTask) {
            bonusTask->run(*this);
            
            // The main thread could still be running, and may run for a while. Unless we clear the task
            // ourselves, we will keep looping around trying to run the task.
            {
                Locker locker { m_heap.m_markingMutex };
                if (m_heap.m_bonusVisitorTask == bonusTask)
                    m_heap.m_bonusVisitorTask = nullptr;
                bonusTask = nullptr;
                m_heap.m_markingConditionVariable.notifyAll();
            }
        } else {
            RELEASE_ASSERT(!isEmpty());
            drain(timeout);
        }
        
        isActive = true;
    }
}

SlotVisitor::SharedDrainResult SlotVisitor::drainInParallel(MonotonicTime timeout)
{
    donateAndDrain(timeout);
    return drainFromShared(MainDrain, timeout);
}

SlotVisitor::SharedDrainResult SlotVisitor::drainInParallelPassively(MonotonicTime timeout)
{
    ASSERT(m_isInParallelMode);
    
    ASSERT(Options::numberOfGCMarkers());
    
    if (!m_heap.m_hasParallelMarkers
        || (m_heap.m_worldState.load() & Heap::mutatorWaitingBit)
        || !m_heap.hasHeapAccess()
        || m_heap.worldIsStopped()) {
        // This is an optimization over drainInParallel() when we have a concurrent mutator but
        // otherwise it is not profitable.
        return drainInParallel(timeout);
    }

    donateAll(Locker { m_heap.m_markingMutex });
    return waitForTermination(timeout);
}

SlotVisitor::SharedDrainResult SlotVisitor::waitForTermination(MonotonicTime timeout)
{
    Locker locker { m_heap.m_markingMutex };
    for (;;) {
        if (hasElapsed(timeout))
            return SharedDrainResult::TimedOut;
        
        if (didReachTermination(locker)) {
            m_heap.m_markingConditionVariable.notifyAll();
            return SharedDrainResult::Done;
        }
        
        m_heap.m_markingConditionVariable.waitUntil(m_heap.m_markingMutex, timeout);
    }
}

void SlotVisitor::donateAll()
{
    if (isEmpty())
        return;
    
    donateAll(Locker { m_heap.m_markingMutex });
}

void SlotVisitor::donateAll(const AbstractLocker&)
{
    forEachMarkStack(
        [&] (MarkStackArray& stack) -> IterationStatus {
            stack.transferTo(correspondingGlobalStack(stack));
            return IterationStatus::Continue;
        });

    m_heap.m_markingConditionVariable.notifyAll();
}

void SlotVisitor::donate()
{
    if (!m_isInParallelMode) {
        dataLog("FATAL: Attempting to donate when not in parallel mode.\n");
        RELEASE_ASSERT_NOT_REACHED();
    }
    
    donateKnownParallel();
}

void SlotVisitor::donateAndDrain(MonotonicTime timeout)
{
    donate();
    drain(timeout);
}

void SlotVisitor::didRace(const VisitRaceKey& race)
{
    dataLogLnIf(Options::verboseVisitRace(), toCString("GC visit race: ", race));
    
    Locker locker { heap()->m_raceMarkStackLock };
    JSCell* cell = race.cell();
    cell->setCellState(CellState::PossiblyGrey);
    heap()->m_raceMarkStack->append(cell);
}

void SlotVisitor::dump(PrintStream& out) const
{
    out.print("Collector: [", pointerListDump(collectorMarkStack()), "], Mutator: [", pointerListDump(mutatorMarkStack()), "]");
}

MarkStackArray& SlotVisitor::correspondingGlobalStack(MarkStackArray& stack)
{
    if (&stack == &m_collectorStack)
        return *m_heap.m_sharedCollectorMarkStack;
    RELEASE_ASSERT(&stack == &m_mutatorStack);
    return *m_heap.m_sharedMutatorMarkStack;
}

NO_RETURN_DUE_TO_CRASH void SlotVisitor::addParallelConstraintTask(RefPtr<SharedTask<void(AbstractSlotVisitor&)>>)
{
    RELEASE_ASSERT_NOT_REACHED();
}

void SlotVisitor::addParallelConstraintTask(RefPtr<SharedTask<void(SlotVisitor&)>> task)
{
    RELEASE_ASSERT(m_currentSolver);
    RELEASE_ASSERT(m_currentConstraint);
    RELEASE_ASSERT(task);

    m_currentSolver->addParallelTask(task, *m_currentConstraint);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
