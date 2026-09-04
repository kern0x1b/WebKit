/*
 * Copyright (C) 2011-2019 Apple Inc. All rights reserved.
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

#import "config.h"
#import <wtf/MemoryPressureHandler.h>

#import <mach/mach.h>
#import <mach/task_info.h>
#import <malloc/malloc.h>
#import <notify.h>
#import <sys/sysctl.h>
#import <wtf/Logging.h>
#import <wtf/MemoryFootprint.h>
#import <wtf/spi/darwin/DispatchSPI.h>

#define ENABLE_FMW_FOOTPRINT_COMPARISON 0

extern "C" void cache_simulate_memory_warning_event(uint64_t);

namespace WTF {

void MemoryPressureHandler::platformReleaseMemory(Critical critical)
{
#if defined(WEBKIT_IOS6)
    // The condition below means "the OS has not told libcache itself, so tell
    // it". Here the OS never tells it: the dispatch memory-pressure source is
    // refused on this system (see install()), and what pressure this port has
    // is derived from polling kern.memorystatus_level and this process's own
    // footprint. isUnderMemoryPressure() being true is therefore not evidence
    // that libcache has heard anything, and reading it here would silence the
    // prod at exactly the Strict threshold where it is wanted.
    if (critical == Critical::Yes)
        cache_simulate_memory_warning_event(DISPATCH_MEMORYPRESSURE_CRITICAL);
#else
    if (critical == Critical::Yes && (!isUnderMemoryPressure() || m_isSimulatingMemoryPressure)) {
        // libcache listens to OS memory notifications, but for process suspension
        // or memory pressure simulation, we need to prod it manually:
        cache_simulate_memory_warning_event(DISPATCH_MEMORYPRESSURE_CRITICAL);
    }
#endif
}

static OSObjectPtr<dispatch_source_t>& NODELETE memoryPressureEventSource()
{
    static NeverDestroyed<OSObjectPtr<dispatch_source_t>> source;
    return source.get();
}

static OSObjectPtr<dispatch_source_t>& NODELETE timerEventSource()
{
    static NeverDestroyed<OSObjectPtr<dispatch_source_t>> source;
    return source.get();
}

// One token for each of the memory pressure/memory warning notifications we listen for.
// notifyutil -p org.WebKit.lowMemory[.begin/.end]
// notifyutil -p org.WebKit.memoryWarning[.begin/.end]
static std::array<int, 6> notifyTokens;

// Disable memory event reception for a minimum of s_minimumHoldOffTime
// seconds after receiving an event. Don't let events fire any sooner than
// s_holdOffMultiplier times the last cleanup processing time. Effectively 
// this is 1 / s_holdOffMultiplier percent of the time.
// These value seems reasonable and testing verifies that it throttles frequent
// low memory events, greatly reducing CPU usage.
static const Seconds s_minimumHoldOffTime { 5_s };
#if !PLATFORM(IOS_FAMILY)
static constexpr unsigned s_holdOffMultiplier = 20;
#endif

#if defined(WEBKIT_IOS6)
static const Seconds s_criticalPressureRepeatInterval { 60_s };

// Percentage of system memory still free, as jetsam itself accounts for it.
// LegacyTileCache reads the same sysctl to size its tile budget.
static int systemMemoryFreeLevel()
{
    int level = 0;
    size_t size = sizeof(level);
    if (sysctlbyname("kern.memorystatus_level", &level, &size, nullptr, 0))
        return 100;
    return level;
}

static size_t processMemoryBudget()
{
    static size_t budget = 0;
    if (!budget) {
        budget = 320 * MB;
        if (const char* override = getenv("WEBKIT_IOS6_MEMORY_BUDGET_MB")) {
            long value = strtol(override, nullptr, 10);
            if (value > 16 && value < 4096)
                budget = static_cast<size_t>(value) * MB;
        }
    }
    return budget;
}

static SystemMemoryPressureStatus gradeMemoryPressure(SystemMemoryPressureStatus previous)
{
    size_t footprint = memoryFootprint();
    size_t budget = processMemoryBudget();
    int level = systemMemoryFreeLevel();

    if (footprint >= budget * 9 / 10 || level < 8)
        return SystemMemoryPressureStatus::Critical;

    if (footprint >= budget * 3 / 4 || level < 12)
        return SystemMemoryPressureStatus::Warning;

    if (previous != SystemMemoryPressureStatus::Normal && footprint >= budget * 5 / 8)
        return SystemMemoryPressureStatus::Warning;

    return SystemMemoryPressureStatus::Normal;
}
#endif

void MemoryPressureHandler::install()
{
    if (m_installed || timerEventSource())
        return;

    dispatch_async(m_dispatchQueue.get(), ^{
#if defined(WEBKIT_IOS6)
        // The graded memory-pressure source is iOS 8, and this system refuses
        // even the ungraded VM pressure source that preceded it — dispatch says
        // so by returning nothing rather than by failing, so nothing here ever
        // fired. kern.memorystatus_level is the pressure signal the kernel does
        // export, and it is what jetsam decides on, so it is polled instead on
        // the interval that already bounds how often pressure may be answered.
        SUPPRESS_RETAINPTR_CTOR_ADOPT memoryPressureEventSource() = adoptOSObject(dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, m_dispatchQueue.get()));
        if (!memoryPressureEventSource())
            return;

        dispatch_source_set_timer(memoryPressureEventSource().get(), dispatch_time(DISPATCH_TIME_NOW, 0),
            s_minimumHoldOffTime.seconds() * NSEC_PER_SEC, NSEC_PER_SEC);
        dispatch_source_set_event_handler(memoryPressureEventSource().get(), ^{
            SystemMemoryPressureStatus previous = m_memoryPressureStatus.load();
            SystemMemoryPressureStatus status = gradeMemoryPressure(previous);
            setMemoryPressureStatus(status);

            if (status == SystemMemoryPressureStatus::Critical) {
                static std::optional<MonotonicTime> lastCriticalResponse;
                auto now = MonotonicTime::now();
                if (!lastCriticalResponse || now - *lastCriticalResponse >= s_criticalPressureRepeatInterval) {
                    lastCriticalResponse = now;
                    respondToMemoryPressure(Critical::Yes);
                }
            } else if (status == SystemMemoryPressureStatus::Warning && previous != SystemMemoryPressureStatus::Warning)
                respondToMemoryPressure(Critical::No);

            if (m_shouldLogMemoryMemoryPressureEvents)
                RELEASE_LOG(MemoryPressure, "Memory pressure: footprint %zu MB, budget %zu MB, system free %d%%, status %d", memoryFootprint() / MB, processMemoryBudget() / MB, systemMemoryFreeLevel(), static_cast<int>(status));
        });
        dispatch_resume(memoryPressureEventSource().get());
#else
        auto memoryStatusFlags = DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL | DISPATCH_MEMORYPRESSURE_PROC_LIMIT_WARN | DISPATCH_MEMORYPRESSURE_PROC_LIMIT_CRITICAL;
        auto *memoryPressureSourceType = DISPATCH_SOURCE_TYPE_MEMORYPRESSURE;
        // FIXME: This is a false positive. rdar://160931336
        SUPPRESS_RETAINPTR_CTOR_ADOPT memoryPressureEventSource() = adoptOSObject(dispatch_source_create(memoryPressureSourceType, 0, memoryStatusFlags, m_dispatchQueue.get()));

        dispatch_source_set_event_handler(memoryPressureEventSource().get(), ^{
            auto status = dispatch_source_get_data(memoryPressureEventSource().get());
            switch (status) {
            // VM pressure events.
            case DISPATCH_MEMORYPRESSURE_NORMAL:
                setMemoryPressureStatus(SystemMemoryPressureStatus::Normal);
                break;
            case DISPATCH_MEMORYPRESSURE_WARN:
                setMemoryPressureStatus(SystemMemoryPressureStatus::Warning);
                respondToMemoryPressure(Critical::No);
                break;
            case DISPATCH_MEMORYPRESSURE_CRITICAL:
                setMemoryPressureStatus(SystemMemoryPressureStatus::Critical);
                respondToMemoryPressure(Critical::Yes);
                break;
            // Process memory limit events.
            case DISPATCH_MEMORYPRESSURE_PROC_LIMIT_WARN:
                didExceedProcessMemoryLimit(ProcessMemoryLimit::Warning);
                respondToMemoryPressure(Critical::No);
                break;
            case DISPATCH_MEMORYPRESSURE_PROC_LIMIT_CRITICAL:
                didExceedProcessMemoryLimit(ProcessMemoryLimit::Critical);
                respondToMemoryPressure(Critical::Yes);
                break;
            }
            if (m_shouldLogMemoryMemoryPressureEvents)
                RELEASE_LOG(MemoryPressure, "Received memory pressure event: %lu, system vm pressure critical: %d", status, isUnderMemoryPressure());
        });
        dispatch_resume(memoryPressureEventSource().get());
#endif
    });

    // Allow simulation of memory warning (80% of high watermark) with "notifyutil -p org.WebKit.memoryWarning
    notify_register_dispatch("org.WebKit.memoryWarning", &notifyTokens[0], m_dispatchQueue.get(), ^(int) {
#if ENABLE(FMW_FOOTPRINT_COMPARISON)
        auto footprintBefore = pagesPerVMTag();
#endif
        beginSimulatedMemoryWarning();

#if ENABLE(FMW_FOOTPRINT_COMPARISON)
        auto footprintAfter = pagesPerVMTag();
        logFootprintComparison(footprintBefore, footprintAfter);
#endif

        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), m_dispatchQueue.get(), ^{
            endSimulatedMemoryWarning();
        });
    });

    notify_register_dispatch("org.WebKit.memoryWarning.begin", &notifyTokens[1], m_dispatchQueue.get(), ^(int) {
        beginSimulatedMemoryWarning();
    });
    notify_register_dispatch("org.WebKit.memoryWarning.end", &notifyTokens[2], m_dispatchQueue.get(), ^(int) {
        endSimulatedMemoryWarning();
    });

    // Allow simulation of memory pressure with "notifyutil -p org.WebKit.lowMemory"
    notify_register_dispatch("org.WebKit.lowMemory", &notifyTokens[3], m_dispatchQueue.get(), ^(int) {
#if ENABLE(FMW_FOOTPRINT_COMPARISON)
        auto footprintBefore = pagesPerVMTag();
#endif
        beginSimulatedMemoryPressure();

        WTF::releaseFastMallocFreeMemory();
        malloc_zone_pressure_relief(nullptr, 0);

#if ENABLE(FMW_FOOTPRINT_COMPARISON)
        auto footprintAfter = pagesPerVMTag();
        logFootprintComparison(footprintBefore, footprintAfter);
#endif

        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), m_dispatchQueue.get(), ^{
            endSimulatedMemoryPressure();
        });
    });

    notify_register_dispatch("org.WebKit.lowMemory.begin", &notifyTokens[4], m_dispatchQueue.get(), ^(int) {
        beginSimulatedMemoryPressure();
    });
    notify_register_dispatch("org.WebKit.lowMemory.end", &notifyTokens[5], m_dispatchQueue.get(), ^(int) {
        endSimulatedMemoryPressure();
    });

    m_installed = true;
}

void MemoryPressureHandler::uninstall()
{
    if (!m_installed)
        return;

    dispatch_async(m_dispatchQueue.get(), ^{
        if (memoryPressureEventSource()) {
            dispatch_source_cancel(memoryPressureEventSource().get());
            memoryPressureEventSource() = nullptr;
        }

        if (timerEventSource()) {
            dispatch_source_cancel(timerEventSource().get());
            timerEventSource() = nullptr;
        }
    });

    m_installed = false;

    for (auto& token : notifyTokens)
        notify_cancel(token);
}

void MemoryPressureHandler::holdOff(Seconds seconds)
{
    dispatch_async(m_dispatchQueue.get(), ^{
        // FIXME: This is a false positive. rdar://160931336
        SUPPRESS_RETAINPTR_CTOR_ADOPT timerEventSource() = adoptOSObject(dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, m_dispatchQueue.get()));
        if (timerEventSource()) {
            dispatch_set_context(timerEventSource().get(), this);
            // FIXME: The final argument `s_minimumHoldOffTime.seconds()` seems wrong.
            // https://bugs.webkit.org/show_bug.cgi?id=183277
            dispatch_source_set_timer(timerEventSource().get(), dispatch_time(DISPATCH_TIME_NOW, seconds.seconds() * NSEC_PER_SEC), DISPATCH_TIME_FOREVER, s_minimumHoldOffTime.seconds());
            dispatch_source_set_event_handler(timerEventSource().get(), ^{
                if (timerEventSource().get()) {
                    dispatch_source_cancel(timerEventSource().get());
                    timerEventSource() = nullptr;
                }
                MemoryPressureHandler::singleton().install();
            });
            dispatch_resume(timerEventSource().get());
        }
    });
}

void MemoryPressureHandler::respondToMemoryPressure(Critical critical, Synchronous synchronous)
{
#if !PLATFORM(IOS_FAMILY)
    uninstall();
    MonotonicTime startTime = MonotonicTime::now();
#endif

    releaseMemory(critical, synchronous);

#if !PLATFORM(IOS_FAMILY)
    Seconds holdOffTime = (MonotonicTime::now() - startTime) * s_holdOffMultiplier;
    holdOff(std::max(holdOffTime, s_minimumHoldOffTime));
#endif
}

std::optional<MemoryPressureHandler::ReliefLogger::MemoryUsage> MemoryPressureHandler::ReliefLogger::platformMemoryUsage()
{
    task_vm_info_data_t vmInfo;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    kern_return_t err = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t) &vmInfo, &count);
    if (err != KERN_SUCCESS)
        return std::nullopt;

    // phys_footprint is past what this kernel fills in, so relief is measured
    // against the same resident size the rest of the port accounts by.
#if defined(WEBKIT_IOS6)
    return MemoryUsage {static_cast<size_t>(vmInfo.internal), memoryFootprint()};
#else
    return MemoryUsage {static_cast<size_t>(vmInfo.internal), static_cast<size_t>(vmInfo.phys_footprint)};
#endif
}

} // namespace WTF

#undef LOG_CHANNEL_PREFIX
