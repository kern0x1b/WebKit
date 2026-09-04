/*
 * Copyright (C) 2021 Apple Inc. All rights reserved.
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
#include "JITWorklistThread.h"

#if ENABLE(JIT)

#include "HeapInlines.h"
#include "JITWorklist.h"
#include "VM.h"
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Threading.h>

namespace JSC {

#if defined(WEBKIT_IOS6)
// Defined in JIT.cpp; declared here rather than pulling in JIT.h, which this file has no other
// reason to include.
namespace CostCeilingInstrumentation {
bool queueOrderingEnabled();
unsigned queueStarveThresholdMS();
unsigned queueDiscardThresholdMS();
void recordQueueReordered();
void recordQueueStarvationPromotion();
void recordQueueDiscarded();
}
#endif

WTF_MAKE_TZONE_ALLOCATED_IMPL(JITWorklistThread);

class JITWorklistThread::WorkScope final {
public:
    WorkScope(JITWorklistThread& thread)
        : m_thread(thread)
        , m_tier(thread.m_plan->tier())
    {
        RELEASE_ASSERT(m_thread->m_plan);
    }

    ~WorkScope()
    {
        Locker locker { *m_thread->m_worklist.m_lock };
        m_thread->m_plan = nullptr;
        m_thread->m_worklist.m_ongoingCompilationsPerTier[static_cast<unsigned>(m_tier)]--;

        ASSERT(m_thread->m_planLoad);
        ASSERT(m_thread->m_worklist.m_totalLoad >= m_thread->m_planLoad);
        m_thread->m_worklist.m_totalLoad -= m_thread->m_planLoad;
        m_thread->m_planLoad = 0;

        ASSERT(!m_thread->m_worklist.m_totalLoad ==
            (!m_thread->m_worklist.queueLength(locker) && !m_thread->m_worklist.totalOngoingCompilations(locker)));
    }

private:
#if USE(PROTECTED_JIT)
    // Must be constructed before we allocate anything using SequesteredArenaMalloc
    ArenaLifetime m_saLifetime { };
#endif
    CheckedRef<JITWorklistThread> m_thread;
    JITPlan::Tier m_tier;
};

#if USE(PROTECTED_JIT_STACKS)
JITWorklistThread::JITWorklistThread(const AbstractLocker& locker, JITWorklist& worklist)
    : SequesteredAutomaticThread(locker, worklist.m_lock, worklist.m_planEnqueued.copyRef())
    , m_worklist(worklist)
{
}
#else
JITWorklistThread::JITWorklistThread(const AbstractLocker& locker, JITWorklist& worklist)
    : AutomaticThread(locker, worklist.m_lock, worklist.m_planEnqueued.copyRef(), ThreadType::Compiler)
    , m_worklist(worklist)
{
}
#endif

ASCIILiteral JITWorklistThread::name() const
{
#if OS(LINUX)
    return "JITWorker"_s;
#else
    return "JIT Worklist Helper Thread"_s;
#endif
}

auto JITWorklistThread::poll(const AbstractLocker& locker) -> PollResult
{
    for (unsigned i = 0; i < static_cast<unsigned>(JITPlan::Tier::Count); ++i) {
        auto& queue = m_worklist.m_queues[i];
        if (queue.isEmpty())
            continue;

        if (m_worklist.m_ongoingCompilationsPerTier[i] >= m_worklist.m_maximumNumberOfConcurrentCompilationsPerTier[i])
            continue;

#if defined(WEBKIT_IOS6)
        // Only the DFG tier is reordered: this device pins numberOfDFGCompilerThreads to 1
        // (see app/native-main.m), so that single thread otherwise drains DFG::Plans in
        // strict enqueue order, meaning a plan queued during page load for code nobody calls
        // anymore sits ahead of one just triggered by a function the user is actively
        // scrolling through. Baseline is untouched (cheap, uncontended); FTL is untouched
        // (never runs on this platform).
        if (i == static_cast<unsigned>(JITPlan::Tier::DFG) && CostCeilingInstrumentation::queueOrderingEnabled()) {
            m_plan = selectAndRemoveBestDFGPlan(queue);
            if (!m_plan) {
                // Eviction alone emptied the queue; nothing here is worth compiling right
                // now. This is not the upstream shutdown sentinel below - just continue on
                // to the next tier / Wait, the same as if the queue had been empty all along.
                continue;
            }
        } else
#endif
        {
            m_plan = queue.takeFirst();
            if (!m_plan) [[unlikely]] {
                if (Options::verboseCompilationQueue()) {
                    m_worklist.dump(locker, WTF::dataFile());
                    dataLog(": Thread shutting down\n");
                }
                return PollResult::Stop;
            }
        }

        RELEASE_ASSERT(m_plan->stage() == JITPlanStage::Preparing);
        // Dequeuing this plan doesn't change the total load yet, but it will once the compilation finishes.
        // If the plan is canceled during compilation, the codeBlock may no longer be alive, so remember the plan's load now.
        m_planLoad = m_worklist.planLoad(*m_plan);
        m_worklist.m_ongoingCompilationsPerTier[i]++;
        return PollResult::Work;
    }
    RELEASE_ASSERT(m_worklist.m_numberOfActiveThreads);
    m_worklist.m_numberOfActiveThreads--;
    return PollResult::Wait;
}

#if defined(WEBKIT_IOS6)
namespace {

// How much each signal is worth when two DFG::Plans are competing for the single compiler
// thread's attention. Reheat dominates because it is the only signal that is refreshed while
// a plan waits (see JITWorklist::removeAllReadyPlansForVM()); the loop-trigger bit is a
// one-shot hint taken at enqueue time (see DFG::Plan's constructor in DFGPlan.cpp), so it only
// breaks ties between plans that haven't reheated yet. The starvation bound is handled
// separately below rather than folded into this score, so it stays an honest bound ("waited
// this long, goes next") instead of a score threshold that would depend on how hot whatever
// else is in the queue happens to be.
constexpr unsigned reheatScoreWeight = 1000;
constexpr unsigned loopTriggerScoreBonus = 1;

unsigned dfgPlanScore(JITPlan& plan)
{
    unsigned score = plan.reheatCountForQueueOrdering() * reheatScoreWeight;
    if (plan.wasLoopTriggerAtEnqueueForQueueOrdering())
        score += loopTriggerScoreBonus;
    return score;
}

} // anonymous namespace

RefPtr<JITPlan> JITWorklistThread::selectAndRemoveBestDFGPlan(Deque<RefPtr<JITPlan>>& queue)
{
    MonotonicTime now = MonotonicTime::now();
    Seconds discardThreshold = Seconds::fromMilliseconds(CostCeilingInstrumentation::queueDiscardThresholdMS());

    // Pass 1: drop plans that are old, have never once proven their code block is still
    // running since they were queued (reheat == 0 - see JITPlan::reheatCountForQueueOrdering()),
    // and were not themselves triggered from inside a loop (a loop trigger is itself strong
    // evidence the code was live when this plan was created, so those are only ever reordered,
    // never discarded - see JITPlan::wasLoopTriggerAtEnqueueForQueueOrdering()). Bounded per
    // poll() call so a large backlog can't turn one dequeue into an unbounded scan under
    // m_worklist.m_lock.
    constexpr unsigned maxEvictionsPerPoll = 8;
    for (unsigned evictions = 0; evictions < maxEvictionsPerPoll; ++evictions) {
        auto it = queue.findIf([&](const RefPtr<JITPlan>& candidate) {
            JITPlan& plan = *candidate;
            if (plan.wasLoopTriggerAtEnqueueForQueueOrdering())
                return false;
            if (plan.reheatCountForQueueOrdering())
                return false;
            return (now - plan.timeCreatedForQueueOrdering()) >= discardThreshold;
        });
        if (it == queue.end())
            break;
        RefPtr<JITPlan> discarded = WTF::move(*it);
        queue.remove(it);
        m_worklist.discardPreparingPlan(discarded.releaseNonNull());
        CostCeilingInstrumentation::recordQueueDiscarded();
    }

    if (queue.isEmpty())
        return nullptr;

    // Pass 2: anything already past the starvation bound goes next, oldest-of-the-starved
    // first, regardless of score - this is the bound on how long a lukewarm plan can be
    // passed over by hotter arrivals (see queueStarveThresholdMS()'s comment in JIT.cpp for
    // the honest statement of what the bound degrades to under backlog). Otherwise pick the
    // highest-scoring plan; a forward scan that only replaces the incumbent on a strictly
    // higher score keeps ties resolved in favor of the older (earlier-queued) plan for free.
    Seconds starveThreshold = Seconds::fromMilliseconds(CostCeilingInstrumentation::queueStarveThresholdMS());
    auto best = queue.begin();
    bool bestStarved = (now - (*best)->timeCreatedForQueueOrdering()) >= starveThreshold;
    unsigned bestScore = dfgPlanScore(**best);
    for (auto it = std::next(queue.begin()); it != queue.end(); ++it) {
        bool starved = (now - (*it)->timeCreatedForQueueOrdering()) >= starveThreshold;
        if (starved != bestStarved) {
            if (!starved)
                continue; // A starved candidate always beats a non-starved one.
            best = it;
            bestStarved = true;
            bestScore = dfgPlanScore(**it);
            continue;
        }
        if (starved)
            continue; // Both starved: keep the earlier (already-'best') of the two.
        unsigned score = dfgPlanScore(**it);
        if (score > bestScore) {
            best = it;
            bestScore = score;
        }
    }

    if (best != queue.begin())
        CostCeilingInstrumentation::recordQueueReordered();
    if (bestStarved)
        CostCeilingInstrumentation::recordQueueStarvationPromotion();

    RefPtr<JITPlan> chosen = WTF::move(*best);
    queue.remove(best);
    return chosen;
}
#endif

auto JITWorklistThread::work() -> WorkResult
{
    WorkScope workScope(*this);

    Locker locker { m_rightToRun };
    {
        Locker locker { *m_worklist.m_lock };
        if (m_plan->stage() == JITPlanStage::Canceled)
            return WorkResult::Continue;
        m_plan->notifyCompiling();
    }
    dataLogLnIf(Options::verboseCompilationQueue(), m_worklist, ": Compiling ", m_plan->key(), " asynchronously");

    // There's no way for the GC to be safepointing since we own rightToRun.
    if (m_plan->vm()->heap.worldIsStopped()) {
        dataLog("Heap is stopped but here we are! (1)\n");
        RELEASE_ASSERT_NOT_REACHED();
    }
    m_plan->compileInThread(this);
    if (m_plan->stage() != JITPlanStage::Canceled) {
        if (m_plan->vm()->heap.worldIsStopped()) {
            dataLog("Heap is stopped but here we are! (2)\n");
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    {
        Locker locker { *m_worklist.m_lock };
        if (m_plan->stage() == JITPlanStage::Canceled)
            return WorkResult::Continue;

        m_plan->notifyReady();

        if (Options::verboseCompilationQueue()) {
            m_worklist.dump(locker, WTF::dataFile());
            dataLog(": Compiled ", m_plan->key(), " asynchronously\n");
        }

        RELEASE_ASSERT(!m_plan->vm()->heap.worldIsStopped());
        m_worklist.m_readyPlans.append(m_plan.releaseNonNull());
        m_worklist.m_planCompiledOrCancelled.notifyAll();
    }

    return WorkResult::Continue;
}

void JITWorklistThread::threadDidStart()
{
#if defined(WEBKIT_IOS6)
    int priorityDelta = Options::priorityDeltaOfDFGCompilerThreads();
    if (priorityDelta < 0)
        Thread::currentSingleton().changePriority(priorityDelta);
#endif

    dataLogLnIf(Options::verboseCompilationQueue(), m_worklist, ": Thread started");

}

void JITWorklistThread::threadIsStopping(const AbstractLocker&)
{
    dataLogLnIf(Options::verboseCompilationQueue(), m_worklist, ": Thread will stop");
    ASSERT(!m_plan);
    m_plan = nullptr;
}

} // namespace JSC

#endif // ENABLE(JIT)
