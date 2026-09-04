/*
 * Copyright (C) 2026 the Revenant WebKit port.
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
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES
 * ARE DISCLAIMED.
 */

#include "config.h"
#include "Scheduling.h"

#include <wtf/TZoneMallocInlines.h>

#if defined(WEBKIT_IOS6)
#include <stdlib.h>
#include <unistd.h>
#include <wtf/MonotonicTime.h>
#endif

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Scheduling);

// Raised by the thread that receives touches, read by the thread running script.
// A plain int is enough: it is written on one thread and read on another, and a
// stale read costs one scheduler decision, not correctness.
extern "C" {
__attribute__((visibility("default"))) volatile int g_webkitIOS6InputPending = 0;
__attribute__((visibility("default"))) volatile int g_webkitIOS6ContinuousInputPending = 0;
}

bool Scheduling::isInputPending(std::optional<Scheduling::IsInputPendingOptions>&& options) const
{
    // Answering consumes the flag.
    //
    // The application raises it when a finger lands and cannot lower it again at
    // the right moment: the main thread returns from delivering the touch long
    // before the thread running script has dispatched it. Consuming it here
    // means the scheduler is told once, which is all it needs - it yields, the
    // touch is delivered, and the flag is not left standing to make every later
    // question answer yes.
    if (g_webkitIOS6InputPending) {
        g_webkitIOS6InputPending = 0;
        return true;
    }
    // Continuous input - a finger still moving - is only reported when the
    // caller says it wants it, as the specification asks.
    return options && options->includeContinuous && g_webkitIOS6ContinuousInputPending;
}

#if defined(WEBKIT_IOS6)

static volatile int g_ios6RenderingUpdatePending = 0;
static MonotonicTime g_ios6LastScrollMovement;

static double ios6SchedulingIntervalFromEnvironment(const char* name, double fallback)
{
    const char* value = getenv(name);
    if (!value || !*value)
        return fallback;
    char* end = nullptr;
    double milliseconds = strtod(value, &end);
    if (end == value || milliseconds < 0)
        return fallback;
    return milliseconds / 1000;
}

static bool ios6ScrollPriorityEnabled()
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = access("/tmp/native-no-scroll-priority", F_OK) != 0 ? 1 : 0;
    return enabled == 1;
}

void ios6NoteScrollMovement()
{
    g_ios6LastScrollMovement = MonotonicTime::now();
}

void ios6SetRenderingUpdatePending(bool pending)
{
    g_ios6RenderingUpdatePending = pending ? 1 : 0;
}

std::optional<Seconds> ios6ScrollTaskBudget()
{
    bool debug = getenv("WEBKIT_IOS6_DEBUG_EXPOSED_RECT");
    if (!ios6ScrollPriorityEnabled()) {
        if (debug) WTFLogAlways("[budget] disabled");
        return std::nullopt;
    }
    if (!g_ios6LastScrollMovement) {
        if (debug) WTFLogAlways("[budget] never moved");
        return std::nullopt;
    }

    static const Seconds scrollWindow { ios6SchedulingIntervalFromEnvironment("WEBKIT_IOS6_SCROLL_WINDOW_MS", 0.250) };
    Seconds age = MonotonicTime::now() - g_ios6LastScrollMovement;
    if (age > scrollWindow) {
        if (debug) WTFLogAlways("[budget] stale age=%.3f window=%.3f", age.seconds(), scrollWindow.seconds());
        return std::nullopt;
    }

    if (g_ios6RenderingUpdatePending) {
        if (debug) WTFLogAlways("[budget] zero, update pending");
        return 0_s;
    }

    if (debug) WTFLogAlways("[budget] granted age=%.3f", age.seconds());
    static const Seconds taskBudget { ios6SchedulingIntervalFromEnvironment("WEBKIT_IOS6_SCROLL_TASK_BUDGET_MS", 0.004) };
    return taskBudget;
}

static bool ios6SchedulingLogEnabled()
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = access("/tmp/native-sched-log", F_OK) == 0 ? 1 : 0;
    return enabled == 1;
}

static unsigned g_ios6SchedRuns;
static unsigned g_ios6SchedScrollPriorityRuns;
static unsigned g_ios6SchedTasksDeferredByScrollPriority;
static unsigned g_ios6SchedLargestScrollBatch;
static MonotonicTime g_ios6SchedLastReport;

static void ios6ReportSchedulingCountersIfDue()
{
    MonotonicTime now = MonotonicTime::now();
    if (g_ios6SchedLastReport && now - g_ios6SchedLastReport < 2_s)
        return;
    g_ios6SchedLastReport = now;
    WTFLogAlways("[sched] %u event loop passes, scroll priority engaged %u times, %u tasks deferred under it, largest scroll batch %u",
        g_ios6SchedRuns, g_ios6SchedScrollPriorityRuns, g_ios6SchedTasksDeferredByScrollPriority, g_ios6SchedLargestScrollBatch);
    g_ios6SchedRuns = 0;
    g_ios6SchedScrollPriorityRuns = 0;
    g_ios6SchedTasksDeferredByScrollPriority = 0;
    g_ios6SchedLargestScrollBatch = 0;
}

void ios6NoteEventLoopRun(unsigned taskBatchSize, bool scrollPriority)
{
    if (!ios6SchedulingLogEnabled())
        return;
    ++g_ios6SchedRuns;
    if (scrollPriority) {
        ++g_ios6SchedScrollPriorityRuns;
        if (taskBatchSize > g_ios6SchedLargestScrollBatch)
            g_ios6SchedLargestScrollBatch = taskBatchSize;
    }
    ios6ReportSchedulingCountersIfDue();
}

void ios6NoteScrollPriorityDeferral()
{
    if (!ios6SchedulingLogEnabled())
        return;
    ++g_ios6SchedTasksDeferredByScrollPriority;
}

#endif

} // namespace WebCore
