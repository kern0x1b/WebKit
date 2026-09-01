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

} // namespace WebCore
