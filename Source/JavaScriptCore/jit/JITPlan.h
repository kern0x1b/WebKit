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

#pragma once

#if ENABLE(JIT)

#include "CompilationResult.h"
#include "JITCode.h"
#include "JITCompilationKey.h"
#include "JITCompilationMode.h"
#include "JITPlanStage.h"
#include "ReleaseHeapAccessScope.h"
#include <wtf/MonotonicTime.h>
#include <wtf/ThreadSafeRefCounted.h>
#if defined(WEBKIT_IOS6)
#include <atomic>
#endif

namespace JSC {

class AbstractSlotVisitor;
class CodeBlock;
class JITWorklistThread;
class VM;

class JITPlan : public ThreadSafeRefCounted<JITPlan> {
protected:
    JITPlan(JITCompilationMode, CodeBlock*);

public:
    virtual ~JITPlan();

    VM* vm() const { return m_vm; }
    CodeBlock* codeBlock() const { return m_codeBlock; }
    JITWorklistThread* thread() const { return m_thread.get(); }

    JITCompilationMode mode() const { return m_mode; }

    JITPlanStage stage() const { return m_stage; }
    bool isDFG() const { return ::JSC::isDFG(m_mode); }
    bool isFTL() const { return ::JSC::isFTL(m_mode); }
    bool isUnlinked() const { return ::JSC::isUnlinked(m_mode); }

    enum class Tier { Baseline = 0, DFG = 1, FTL = 2, Count = 3 };
    Tier NODELETE tier() const;
    JITType jitType() const
    {
        switch (tier()) {
        case Tier::Baseline:
            return JITType::BaselineJIT;
        case Tier::DFG:
            return JITType::DFGJIT;
        case Tier::FTL:
            return JITType::FTLJIT;
        default:
            return JITType::None;
        }
    }

    JITCompilationKey key();

#if defined(WEBKIT_IOS6)
    // Read by JITWorklistThread::poll()'s dequeue-time reordering (see JITWorklistThread.cpp)
    // and by JITWorklist::removeAllReadyPlansForVM() (see JITWorklist.cpp), never by anything
    // upstream. See the field declarations below for what each one means; all are inert
    // (false/0/zero-time) unless WEBKIT_IOS6_DFG_QUEUE_HOTTEST_FIRST is set, so this costs
    // nothing when the switch is off.
    MonotonicTime timeCreatedForQueueOrdering() const { return m_timeCreatedForQueueOrdering; }
    bool wasLoopTriggerAtEnqueueForQueueOrdering() const { return m_wasLoopTriggerAtEnqueue; }
    unsigned reheatCountForQueueOrdering() const { return m_reheatCountForQueueOrdering.load(std::memory_order_relaxed); }
    // Called from JITWorklist::removeAllReadyPlansForVM() while this plan is still Preparing
    // or Compiling, every time the code block it targets re-crosses its (re-armed) tier-up
    // threshold - i.e. every time there's fresh proof the code kept running after this plan
    // was queued. See JITWorklist.cpp for the call site and JITOperations.cpp's
    // operationOptimize() for why re-crossing while a plan is outstanding is possible at all
    // (CodeBlock::setOptimizationThresholdBasedOnCompilationResult(CompilationDeferred)).
    void bumpReheatForQueueOrdering() { m_reheatCountForQueueOrdering.fetch_add(1, std::memory_order_relaxed); }
#endif

    void compileInThread(JITWorklistThread*);

    virtual size_t codeSize() const = 0;

    virtual CompilationResult finalize() = 0;

    virtual void finalizeInGC() { }

    void notifyCompiling();
    virtual void notifyReady();
    virtual void cancel();

    virtual bool isKnownToBeLiveAfterGC();
    virtual bool isKnownToBeLiveDuringGC(AbstractSlotVisitor&);
    virtual bool iterateCodeBlocksForGC(AbstractSlotVisitor&, NOESCAPE const Function<void(CodeBlock*)>&);
    virtual bool checkLivenessAndVisitChildren(AbstractSlotVisitor&);

    bool NODELETE isInSafepoint() const;
    bool NODELETE safepointKeepsDependenciesLive() const;

    template<typename Functor>
    void addMainThreadFinalizationTask(const Functor& functor)
    {
        m_mainThreadFinalizationTasks.append(createSharedTask<void()>(functor));
    }

    void runMainThreadFinalizationTasks();

    enum class SignpostDetail { None, Canceled };

    CString signpostMessage();

    void beginSignpost()
    {
        if (Options::useCompilerSignpost()) [[unlikely]]
            beginSignpostImpl();
    }

    void endSignpost(SignpostDetail detail = SignpostDetail::None)
    {
        if (Options::useCompilerSignpost()) [[unlikely]]
            endSignpostImpl(detail);
    }

protected:
    bool NODELETE computeCompileTimes() const;
    bool NODELETE reportCompileTimes() const;

    enum CompilationPath { FailPath, BaselinePath, DFGPath, FTLPath, CancelPath };
    virtual CompilationPath compileInThreadImpl() = 0;

    void beginSignpostImpl();
    void endSignpostImpl(SignpostDetail);

    JITPlanStage m_stage { JITPlanStage::Preparing };
    JITCompilationMode m_mode;
    MonotonicTime m_timeBeforeFTL;
#if defined(WEBKIT_IOS6)
    // Construction time, used as a stand-in for worklist-enqueue time (the two happen back to
    // back with nothing blocking in between) to measure how long a DFG plan waits for the
    // single numberOfDFGCompilerThreads=1 slot before a worker thread dispatches it. Left at
    // its zero default (no clock read - every JITPlan, including every baseline compile, goes
    // through this constructor) unless instrumentation is on; set from the constructor body in
    // JITPlan.cpp instead of here for exactly that reason. See JITPlan::compileInThread() and
    // CostCeilingInstrumentation::recordDFGQueueAge() in JIT.cpp.
    MonotonicTime m_timeCreatedForQueueInstrumentation;

    // Separate switch from the one above on purpose: WEBKIT_IOS6_OPT_CEILING_LOG (the age
    // histogram) and WEBKIT_IOS6_DFG_QUEUE_HOTTEST_FIRST (this) are independent so a run can
    // log with reordering off (baseline) and log with reordering on (A/B) using the same
    // enable-the-log knob. Construction time, gated on the reorder switch; used by
    // JITWorklistThread::poll() to compute how long a plan has waited and whether it has
    // crossed the starvation bound or the discard age (see JITWorklistThread.cpp). Left at
    // its zero default unless the switch is on. Set from the constructor body in JITPlan.cpp
    // for the same reason as m_timeCreatedForQueueInstrumentation above.
    MonotonicTime m_timeCreatedForQueueOrdering;

    // Whether this plan's construction was triggered from inside a loop (a valid OSR-entry
    // bytecode index) rather than from a function-entry call-count trigger. Set once, in
    // DFG::Plan's constructor (DFGPlan.cpp), from the same osrEntryBytecodeIndex that
    // DFG::Plan::osrEntryBytecodeIndex() exposes - a loop trigger is itself strong evidence
    // the code was actually running at the moment this plan was queued, independent of
    // whatever it does afterwards. Always false for plans that aren't DFG::Plan (baseline
    // compiles have no such concept). Never written outside construction.
    bool m_wasLoopTriggerAtEnqueue { false };

    // How many times, while this plan sat Preparing or Compiling, its code block re-crossed
    // its tier-up threshold again - i.e. how many times there was fresh proof the code kept
    // running after this plan was queued. See bumpReheatForQueueOrdering() above and
    // JITWorklist::removeAllReadyPlansForVM() in JITWorklist.cpp for the writer. Atomic
    // because the writer runs on the VM's execution thread while the reader
    // (JITWorklistThread::poll()) runs on a compiler thread; both only ever touch this under
    // JITWorklist::m_lock except for the increment itself, which needs no ordering beyond
    // "eventually visible" since it is advisory (a missed increment just means a plan looks
    // one reheat colder than it is, not a correctness problem).
    std::atomic<unsigned> m_reheatCountForQueueOrdering { 0 };
#endif
    VM* m_vm;
    CodeBlock* m_codeBlock;
    CheckedPtr<JITWorklistThread> m_thread;
    Vector<Ref<SharedTask<void()>>> m_mainThreadFinalizationTasks;
    CString m_signpostMessage; // Non-null iff Options::useCompilerSignpost()
};

} // namespace JSC

#endif // ENABLE(JIT)
