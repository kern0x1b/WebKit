/*
 * Copyright (C) 2013-2020 Apple Inc. All rights reserved.
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
#include "DFGCommon.h"

#include "FunctionAllowlist.h"
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/PrintStream.h>

#if ENABLE(DFG_JIT)

namespace JSC { namespace DFG {

FunctionAllowlist& ensureGlobalDFGAllowlist()
{
    static LazyNeverDestroyed<FunctionAllowlist> dfgAllowlist;
    static std::once_flag initializeAllowlistFlag;
    std::call_once(initializeAllowlistFlag, [] {
        const char* functionAllowlistFile = Options::dfgAllowlist();
        dfgAllowlist.construct(functionAllowlistFile);
    });
    return dfgAllowlist;
}

#if ENABLE(FTL_JIT)
FunctionAllowlist& ensureGlobalFTLAllowlist()
{
    static LazyNeverDestroyed<FunctionAllowlist> ftlAllowlist;
    static std::once_flag initializeAllowlistFlag;
    std::call_once(initializeAllowlistFlag, [] {
        const char* functionAllowlistFile = Options::ftlAllowlist();
        ftlAllowlist.construct(functionAllowlistFile);
    });
    return ftlAllowlist;
}
#endif

#if defined(WEBKIT_IOS6)
static unsigned envUnsigned(const char* name, unsigned defaultValue)
{
    const char* value = getenv(name);
    if (!value || !value[0])
        return defaultValue;
    char* end = nullptr;
    unsigned long parsed = strtoul(value, &end, 10);
    if (end == value)
        return defaultValue;
    return static_cast<unsigned>(parsed);
}

static bool envBool(const char* name, bool defaultValue)
{
    const char* value = getenv(name);
    if (!value || !value[0])
        return defaultValue;
    if (!strcmp(value, "0") || !strcmp(value, "false") || !strcmp(value, "off"))
        return false;
    return true;
}

const PipelineTuning& pipelineTuning()
{
    static PipelineTuning tuning;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        tuning.preciseLocalCSEBlockLimit = envUnsigned("WEBKIT_IOS6_DFG_CSE_PRECISE_BLOCK_LIMIT", 800);
        tuning.typeCheckHoisting = envBool("WEBKIT_IOS6_DFG_TYPE_CHECK_HOISTING", true);
        tuning.staticExecutionCountEstimation = envBool("WEBKIT_IOS6_DFG_STATIC_EXECUTION_COUNTS", true);
        tuning.localCSE = envBool("WEBKIT_IOS6_DFG_LOCAL_CSE", true);
        tuning.strengthReduction = envBool("WEBKIT_IOS6_DFG_STRENGTH_REDUCTION", true);
        tuning.varargsForwarding = envBool("WEBKIT_IOS6_DFG_VARARGS_FORWARDING", true);
    });
    return tuning;
}
#endif

static Lock crashLock;

// Use WTF_IGNORES_THREAD_SAFETY_ANALYSIS since the function keeps holding the lock when returning.
void startCrashing() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    crashLock.lock();
}

bool isCrashing()
{
    return crashLock.isLocked();
}

bool stringLessThan(StringImpl& a, StringImpl& b)
{
    unsigned minLength = std::min(a.length(), b.length());
    for (unsigned i = 0; i < minLength; ++i) {
        if (a[i] == b[i])
            continue;
        return a[i] < b[i];
    }
    return a.length() < b.length();
}

} } // namespace JSC::DFG

namespace WTF {

using namespace JSC::DFG;

void printInternal(PrintStream& out, OptimizationFixpointState state)
{
    switch (state) {
    case BeforeFixpoint:
        out.print("BeforeFixpoint");
        return;
    case FixpointNotConverged:
        out.print("FixpointNotConverged");
        return;
    case FixpointConverged:
        out.print("FixpointConverged");
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void printInternal(PrintStream& out, GraphForm form)
{
    switch (form) {
    case LoadStore:
        out.print("LoadStore");
        return;
    case ThreadedCPS:
        out.print("ThreadedCPS");
        return;
    case SSA:
        out.print("SSA");
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void printInternal(PrintStream& out, UnificationState state)
{
    switch (state) {
    case LocallyUnified:
        out.print("LocallyUnified");
        return;
    case GloballyUnified:
        out.print("GloballyUnified");
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void printInternal(PrintStream& out, RefCountState state)
{
    switch (state) {
    case EverythingIsLive:
        out.print("EverythingIsLive");
        return;
    case ExactRefCount:
        out.print("ExactRefCount");
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void printInternal(PrintStream& out, ProofStatus status)
{
    switch (status) {
    case IsProved:
        out.print("IsProved");
        return;
    case NeedsCheck:
        out.print("NeedsCheck");
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace WTF

#endif // ENABLE(DFG_JIT)

namespace WTF {

using namespace JSC::DFG;

void printInternal(PrintStream& out, CapabilityLevel capabilityLevel)
{
    switch (capabilityLevel) {
    case CannotCompile:
        out.print("CannotCompile");
        return;
    case CanCompile:
        out.print("CanCompile");
        return;
    case CanCompileAndInline:
        out.print("CanCompileAndInline");
        return;
    case CapabilityLevelNotSet:
        out.print("CapabilityLevelNotSet");
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace WTF

