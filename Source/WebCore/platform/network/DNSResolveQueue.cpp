/*
 * Copyright (C) 2009-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2012 Igalia S.L.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
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
#include "DNSResolveQueue.h"

#if USE(GLIB)
#include "DNSResolveQueueGLib.h"
#elif USE(CURL)
#include "DNSResolveQueueCurl.h"
#elif USE(CF)
#include "DNSResolveQueueCFNet.h"
#endif

#include <stdlib.h>
#include <wtf/CompletionHandler.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(DNSResolveQueue);

#if defined(WEBKIT_IOS6)
static int webkitIOS6DNSLimit(const char* name, int defaultValue)
{
    if (const char* override = getenv(name)) {
        int value = atoi(override);
        if (value >= 0 && value <= 64)
            return value;
    }
    return defaultValue;
}
#endif

#if defined(WEBKIT_IOS6)
static const int gNamesToResolveImmediately = webkitIOS6DNSLimit("WEBKIT_IOS6_DNS_IMMEDIATE", 1);
#else
static const int gNamesToResolveImmediately = 4;
#endif

// Coalesce prefetch requests for this long before sending them out.
static const Seconds coalesceDelay { 1_s };

#if defined(WEBKIT_IOS6)
static const int gMaxSimultaneousRequests = webkitIOS6DNSLimit("WEBKIT_IOS6_DNS_MAX_IN_FLIGHT", 2);
#else
static const int gMaxSimultaneousRequests = 8;
#endif

// For a page has links to many outside sites, it is likely that the system DNS resolver won't be able to cache them all anyway, and we don't want
// to negatively affect other applications' performance by pushing their cached entries out.
// If we end up with lots of names to prefetch, some will be dropped.
static const int gMaxRequestsToQueue = 64;

// If there were queued names that couldn't be sent simultaneously, check the state of resolvers after this delay.
static const Seconds resolvingRetryDelay { 100_ms };

DNSResolveQueue& DNSResolveQueue::singleton()
{
    static NeverDestroyed<DNSResolveQueuePlatform> queue;

    return queue;
}

DNSResolveQueue::DNSResolveQueue()
    : m_timer(*this, &DNSResolveQueue::timerFired)
    , m_requestsInFlight(0)
{
    // isUsingProxy will return the initial value of m_isUsingProxy at first on
    // platforms that have an asynchronous implementation of updateIsUsingProxy,
    // so initialize it to true so we won't prefetch before we know if we are using a proxy.
}

// Don't do DNS prefetch if proxies are involved. For many proxy types, the user agent is never
// exposed to the IP address during normal operation. Querying an internal DNS server may not help
// performance, as it doesn't necessarily look up the actual external IP. Also, if DNS returns a
// fake internal address, local caches may keep it even after re-connecting to another network.
bool DNSResolveQueue::isUsingProxy()
{
    MonotonicTime time = MonotonicTime::now();
    static const Seconds minimumProxyCheckDelay = 5_s;
    if (time - m_lastProxyEnabledStatusCheckTime > minimumProxyCheckDelay) {
        m_lastProxyEnabledStatusCheckTime = time;
        updateIsUsingProxy();
    }
    return m_isUsingProxy;
}

void DNSResolveQueue::add(const String& hostname)
{
    // If there are no names queued, and few enough are in flight, resolve immediately (the mouse may be over a link).
    if (!m_names.size()) {
        if (isUsingProxy())
            return;
        if (++m_requestsInFlight <= gNamesToResolveImmediately) {
            platformResolve(hostname);
            return;
        }
        --m_requestsInFlight;
    }

    // It's better to not prefetch some names than to clog the queue.
    // Dropping the newest names, because on a single page, these are likely to be below oldest ones.
    if (m_names.size() < gMaxRequestsToQueue) {
        m_names.add(hostname);
        if (!m_timer.isActive())
            m_timer.startOneShot(coalesceDelay);
    }
}

void DNSResolveQueue::timerFired()
{
    if (isUsingProxy()) {
        m_names.clear();
        return;
    }

    int requestsAllowed = gMaxSimultaneousRequests - m_requestsInFlight;

    for (; !m_names.isEmpty() && requestsAllowed > 0; --requestsAllowed) {
        ++m_requestsInFlight;
        HashSet<String>::iterator currentName = m_names.begin();
        platformResolve(*currentName);
        m_names.remove(currentName);
    }

    if (!m_names.isEmpty())
        m_timer.startOneShot(resolvingRetryDelay);
}

} // namespace WebCore
