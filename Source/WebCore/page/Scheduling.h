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

#pragma once

#include <WebCore/PlatformExportMacros.h>
#include <optional>
#include <wtf/RefCounted.h>
#include <wtf/Seconds.h>
#include <wtf/TZoneMalloc.h>

namespace WebCore {

// Whether a touch is waiting to be delivered to the page.
//
// A scheduler that keeps working across tasks - React's, which this device's
// heaviest pages are built on - asks this before deciding to continue. Without
// an answer it works to its own deadline, and on this machine a single such
// stretch was measured at eight seconds, during which a tap does nothing at all.
//
// The answer cannot come from script: while the page is inside that stretch,
// nothing else in the page runs, including a listener that would record the
// touch. It comes from the thread that receives touches, through a flag the
// application raises the moment one arrives and lowers when it has been
// delivered.
class Scheduling final : public RefCounted<Scheduling> {
    WTF_MAKE_TZONE_ALLOCATED(Scheduling);
public:
    // The generated bindings look for the dictionary inside the interface that
    // declares it, so it lives here rather than beside the class.
    struct IsInputPendingOptions {
        bool includeContinuous { false };
    };

    static Ref<Scheduling> create() { return adoptRef(*new Scheduling); }

    bool isInputPending(std::optional<IsInputPendingOptions>&&) const;

private:
    Scheduling() = default;
};

#if defined(WEBKIT_IOS6)
WEBCORE_EXPORT void ios6NoteScrollMovement();
WEBCORE_EXPORT void ios6SetRenderingUpdatePending(bool);
WEBCORE_EXPORT std::optional<Seconds> ios6ScrollTaskBudget();
WEBCORE_EXPORT void ios6NoteEventLoopRun(unsigned taskBatchSize, bool scrollPriority);
WEBCORE_EXPORT void ios6NoteScrollPriorityDeferral();
#endif

} // namespace WebCore
