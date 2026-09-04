/*
 * Copyright (C) 2015-2023 Apple Inc. All rights reserved.
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
#import "NetworkStorageSession.h"

#import "ClientOrigin.h"
#import "Cookie.h"
#import "CookieRequestHeaderFieldProxy.h"
#import "CookieStorageObserver.h"
#import "CookieStoreGetOptions.h"
#import "HTTPCookieAcceptPolicyCocoa.h"
#import "ResourceRequest.h"
#import "SameSiteInfo.h"
#import <algorithm>
#import <optional>
#import <pal/spi/cf/CFNetworkSPI.h>
#import <wtf/BlockObjCExceptions.h>
#import <wtf/BlockPtr.h>
#import <wtf/CallbackAggregator.h>
#import <wtf/ProcessPrivilege.h>
#import <wtf/URL.h>
#import <wtf/cocoa/TypeCastsCocoa.h>
#import <wtf/cocoa/VectorCocoa.h>
#import <wtf/darwin/DispatchExtras.h>
#import <wtf/text/MakeString.h>
#import <wtf/text/StringBuilder.h>
#import <wtf/text/cf/StringConcatenateCF.h>

@interface NSURL ()
- (CFURLRef)_cfurl;
@end

namespace WebCore {

NetworkStorageSession::~NetworkStorageSession()
{
#if HAVE(COOKIE_CHANGE_LISTENER_API)
    unregisterCookieChangeListenersIfNecessary();
#endif
    clearCookiesVersionChangeCallbacks();
}

void NetworkStorageSession::setCookie(const Cookie& cookie)
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies) || m_isInMemoryCookieStore);

    BEGIN_BLOCK_OBJC_EXCEPTIONS
    [nsCookieStorage() setCookie:cookie.createNSHTTPCookie().get()];
    END_BLOCK_OBJC_EXCEPTIONS
}

void NetworkStorageSession::setCookie(const Cookie& cookie, const URL& url, const URL& mainDocumentURL)
{
    setCookies({ cookie }, url, mainDocumentURL);
}

void NetworkStorageSession::setCookies(const Vector<Cookie>& cookies, const URL& url, const URL& mainDocumentURL)
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies) || m_isInMemoryCookieStore);

    BEGIN_BLOCK_OBJC_EXCEPTIONS
    auto nsCookies = createNSArray(cookies, [] (auto& cookie) -> NSHTTPCookie * {
        return cookie.createNSHTTPCookie().autorelease();
    });

    [nsCookieStorage() setCookies:nsCookies.get() forURL:url.createNSURL().get() mainDocumentURL:mainDocumentURL.createNSURL().get()];
    END_BLOCK_OBJC_EXCEPTIONS
}

void NetworkStorageSession::deleteCookie(const Cookie& cookie, CompletionHandler<void()>&& completionHandler)
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies) || m_isInMemoryCookieStore);

    auto work = [completionHandler = WTF::move(completionHandler), cookieStorage = RetainPtr { nsCookieStorage() }, cookie = cookie.createNSHTTPCookie()] () mutable {
        [cookieStorage deleteCookie:cookie.get()];
        ensureOnMainThread(WTF::move(completionHandler));
    };

    if (m_isInMemoryCookieStore)
        return work();
    dispatch_async(globalDispatchQueueSingleton(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), makeBlockPtr(WTF::move(work)).get());
}

static Vector<Cookie> nsCookiesToCookieVector(NSArray<NSHTTPCookie *> *nsCookies, NOESCAPE const Function<bool(NSHTTPCookie *)>& filter = { })
{
    Vector<Cookie> cookies;
    cookies.reserveInitialCapacity(nsCookies.count);
    for (NSHTTPCookie *nsCookie in nsCookies) {
        @autoreleasepool {
            if (!filter || filter(nsCookie))
                cookies.append(nsCookie);
        }
    }
    if (filter)
        cookies.shrinkToFit();
    return cookies;
}

Vector<Cookie> NetworkStorageSession::getAllCookies()
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies));
    return nsCookiesToCookieVector(retainPtr([nsCookieStorage() cookies]).get());
}

Vector<Cookie> NetworkStorageSession::getCookies(const URL& url)
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies));
    return nsCookiesToCookieVector(retainPtr([nsCookieStorage() cookiesForURL:url.createNSURL().get()]).get());
}

void NetworkStorageSession::hasCookies(const RegistrableDomain& domain, CompletionHandler<void(bool)>&& completionHandler) const
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies));

    bool hasCookieForDomain = false;
    
    for (NSHTTPCookie *nsCookie in [nsCookieStorage() cookies]) {
        if (RegistrableDomain::uncheckedCreateFromHost(nsCookie.domain) == domain) {
            hasCookieForDomain = true;
            break;
        }
    }

    // FIXME: rdar://168454473 (Remove workaround in CookieStorageObserver once CFNetwork bug is resolved)
    if (m_cookieStorageObserver && cookieStorage().get())
        protect(cookieStorageObserver())->registerInternalsForNotifications(true);

    completionHandler(hasCookieForDomain);
}

void NetworkStorageSession::setAllCookiesToSameSiteStrict(const RegistrableDomain& domain, CompletionHandler<void()>&& completionHandler)
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies));

#if defined(WEBKIT_IOS6)
    // SameSite does not exist on this CFNetwork at all: -sameSitePolicy is absent
    // from NSHTTPCookie, and the jar has no field to record a policy in, so there
    // is no such thing here as the Strict cookie this is asked to convert cookies
    // into. Reading the policy raised, WebKit swallowed it, and the loop below
    // never ran. Rewriting every cookie with a "SameSitePolicy" property that this
    // jar discards would only make the code look like it did something. Report the
    // operation as finished, which is what the caller expects when there is
    // nothing to convert.
    UNUSED_PARAM(domain);
    return completionHandler();
#else
    RetainPtr<NSMutableArray<NSHTTPCookie *>> oldCookiesToDelete = adoptNS([[NSMutableArray alloc] init]);
    RetainPtr<NSMutableArray<NSHTTPCookie *>> newCookiesToAdd = adoptNS([[NSMutableArray alloc] init]);

    for (NSHTTPCookie *nsCookie in [nsCookieStorage() cookies]) {
        if (RegistrableDomain::uncheckedCreateFromHost(nsCookie.domain) == domain && nsCookie.sameSitePolicy != NSHTTPCookieSameSiteStrict) {
            [oldCookiesToDelete addObject:nsCookie];
            RetainPtr<NSMutableDictionary<NSHTTPCookiePropertyKey, id>> mutableProperties = adoptNS([[nsCookie properties] mutableCopy]);
            mutableProperties.get()[NSHTTPCookieSameSitePolicy] = NSHTTPCookieSameSiteStrict;
            RetainPtr strictCookie = adoptNS([[NSHTTPCookie alloc] initWithProperties:mutableProperties.get()]);
            [newCookiesToAdd addObject:strictCookie.get()];
        }
    }

    auto aggregator = CallbackAggregator::create([completionHandler = WTF::move(completionHandler), newCookiesToAdd = WTF::move(newCookiesToAdd), cookieStorage = RetainPtr { nsCookieStorage() }] () mutable {
        BEGIN_BLOCK_OBJC_EXCEPTIONS
        for (NSHTTPCookie *newCookie in newCookiesToAdd.get())
            [cookieStorage setCookie:newCookie];
        END_BLOCK_OBJC_EXCEPTIONS
        completionHandler();
    });

    BEGIN_BLOCK_OBJC_EXCEPTIONS
    for (NSHTTPCookie *oldCookie in oldCookiesToDelete.get())
        deleteHTTPCookie(cookieStorage().get(), oldCookie, [aggregator] { });
    END_BLOCK_OBJC_EXCEPTIONS
#endif
}

// -_initWithCFHTTPCookieStorage: does not exist on this Foundation, and this
// CFNetwork has exactly one cookie storage, so a jar scoped to one session is
// not a thing that can be made here at all. The shared jar is the storage every
// other path on this port already ends up using, so hand that back rather than
// raising an exception WebKit will swallow and leave the caller having done
// nothing.
static RetainPtr<NSHTTPCookieStorage> wrapCookieStorage(CFHTTPCookieStorageRef storage)
{
#if defined(WEBKIT_IOS6)
    UNUSED_PARAM(storage);
    return [NSHTTPCookieStorage sharedHTTPCookieStorage];
#else
    return adoptNS([[NSHTTPCookieStorage alloc] _initWithCFHTTPCookieStorage:storage]);
#endif
}

RetainPtr<NSHTTPCookieStorage> NetworkStorageSession::nsCookieStorage() const
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies) || m_isInMemoryCookieStore);
    auto cfCookieStorage = cookieStorage();
    ASSERT(cfCookieStorage || !m_isInMemoryCookieStore);
    if (!m_isInMemoryCookieStore && (!cfCookieStorage || [NSHTTPCookieStorage sharedHTTPCookieStorage]._cookieStorage == cfCookieStorage))
        return [NSHTTPCookieStorage sharedHTTPCookieStorage];

    return wrapCookieStorage(cfCookieStorage.get());
}

CookieStorageObserver& NetworkStorageSession::cookieStorageObserver() const
{
    if (!m_cookieStorageObserver)
        m_cookieStorageObserver = makeUnique<CookieStorageObserver>(nsCookieStorage().get());

    return *m_cookieStorageObserver;
}

RetainPtr<CFURLStorageSessionRef> createPrivateStorageSession(CFStringRef identifier, std::optional<HTTPCookieAcceptPolicy> cookieAcceptPolicy, NetworkStorageSession::ShouldDisableCFURLCache shouldDisableCFURLCache)
{
    const void* sessionPropertyKeys[] = { _kCFURLStorageSessionIsPrivate };
    const void* sessionPropertyValues[] = { kCFBooleanTrue };
    auto sessionProperties = adoptCF(CFDictionaryCreate(kCFAllocatorDefault, sessionPropertyKeys, sessionPropertyValues, sizeof(sessionPropertyKeys) / sizeof(*sessionPropertyKeys), &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    auto storageSession = adoptCF(_CFURLStorageSessionCreate(kCFAllocatorDefault, identifier, sessionProperties.get()));

    if (!storageSession)
        return nullptr;

    if (shouldDisableCFURLCache == NetworkStorageSession::ShouldDisableCFURLCache::Yes)
        _CFURLStorageSessionDisableCache(storageSession.get());

    // The private storage session should have the same properties as the default storage session,
    // with the exception that it should be in-memory only storage.

    // FIXME 9199649: If any of the storages do not exist, do no use the storage session.
    // This could occur if there is an issue figuring out where to place a storage on disk (e.g. the
    // sandbox does not allow CFNetwork access).

    if (shouldDisableCFURLCache == NetworkStorageSession::ShouldDisableCFURLCache::No) {
        auto cache = adoptCF(_CFURLStorageSessionCopyCache(kCFAllocatorDefault, storageSession.get()));
        if (!cache)
            return nullptr;

        CFURLCacheSetMemoryCapacity(cache.get(), [[NSURLCache sharedURLCache] memoryCapacity]);
    }

    auto cookieStorage = adoptCF(_CFURLStorageSessionCopyCookieStorage(kCFAllocatorDefault, storageSession.get()));
    if (!cookieStorage)
        return nullptr;

    NSHTTPCookieAcceptPolicy nsCookieAcceptPolicy;
    if (cookieAcceptPolicy)
        nsCookieAcceptPolicy = toNSHTTPCookieAcceptPolicy(*cookieAcceptPolicy);
    else
        nsCookieAcceptPolicy = [[NSHTTPCookieStorage sharedHTTPCookieStorage] cookieAcceptPolicy];

    // FIXME: Use _CFHTTPCookieStorageGetDefault when USE(CFNETWORK) is defined in WebKit for consistency.
    CFHTTPCookieStorageSetCookieAcceptPolicy(cookieStorage.get(), nsCookieAcceptPolicy);

    return storageSession;
}

RetainPtr<NSArray> NetworkStorageSession::httpCookies(CFHTTPCookieStorageRef cookieStorage) const
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies) || m_isInMemoryCookieStore);
    if (!cookieStorage) {
        RELEASE_ASSERT(!m_isInMemoryCookieStore);
        return [[NSHTTPCookieStorage sharedHTTPCookieStorage] cookies];
    }
    
    auto cookies = adoptCF(CFHTTPCookieStorageCopyCookies(cookieStorage));
    return [NSHTTPCookie _cf2nsCookies:cookies.get()];
}

void NetworkStorageSession::deleteHTTPCookie(CFHTTPCookieStorageRef cookieStorage, NSHTTPCookie *cookie, CompletionHandler<void()>&& completionHandler) const
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies) || m_isInMemoryCookieStore);
    
    auto work = [completionHandler = WTF::move(completionHandler), cookieStorage = RetainPtr { cookieStorage }, cookie = RetainPtr { cookie }, isInMemoryCookieStore = m_isInMemoryCookieStore] () mutable {
        if (!cookieStorage) {
            RELEASE_ASSERT(!isInMemoryCookieStore);
            [[NSHTTPCookieStorage sharedHTTPCookieStorage] deleteCookie:cookie.get()];
        } else
            CFHTTPCookieStorageDeleteCookie(cookieStorage.get(), [cookie _GetInternalCFHTTPCookie]);
        ensureOnMainThread(WTF::move(completionHandler));
    };

    if (m_isInMemoryCookieStore)
        return work();
    dispatch_async(globalDispatchQueueSingleton(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), makeBlockPtr(WTF::move(work)).get());
}

// Every caller of this is now behind #if !defined(WEBKIT_IOS6): the dictionary it
// builds is the private policyProperties argument of -_getCookiesForURL:... and
// -_setCookies:..., and it carries SameSite and partition, none of which exist on
// this CFNetwork. Nothing here can consume it, so it is not built here.
#if !defined(WEBKIT_IOS6)
static RetainPtr<NSDictionary> policyProperties(const SameSiteInfo& sameSiteInfo, NSURL *url, NSString *partition, ThirdPartyCookieBlockingDecision thirdPartyCookieBlockingDecision)
{
#if ENABLE(OPT_IN_PARTITIONED_COOKIES) && defined(CFN_COOKIE_ACCEPTS_POLICY_PARTITION) && CFN_COOKIE_ACCEPTS_POLICY_PARTITION
    BOOL shouldAllowOnlyPartitioned = thirdPartyCookieBlockingDecision == ThirdPartyCookieBlockingDecision::AllExceptPartitioned;
    RetainPtr policyProperties = adoptNS([[NSMutableDictionary alloc] init]);
    policyProperties.get()[@"_kCFHTTPCookiePolicyPropertySiteForCookies"] = RetainPtr { sameSiteInfo.isSameSite ? url : URL::emptyNSURL() };
    policyProperties.get()[@"_kCFHTTPCookiePolicyPropertyIsTopLevelNavigation"] = [NSNumber numberWithBool:sameSiteInfo.isTopSite];
    policyProperties.get()[@"_kCFHTTPCookiePolicyPropertyAllowOnlyPartitionedCookies"] = @(shouldAllowOnlyPartitioned);
    if (partition)
        policyProperties.get()[@"_kCFHTTPCookiePolicyPropertyStoragePartitionIdentifier"] = partition;
#else
    UNUSED_PARAM(partition);
    UNUSED_PARAM(thirdPartyCookieBlockingDecision);
    NSDictionary *policyProperties = @{
        @"_kCFHTTPCookiePolicyPropertySiteForCookies": sameSiteInfo.isSameSite ? url : URL::emptyNSURL(),
        @"_kCFHTTPCookiePolicyPropertyIsTopLevelNavigation": [NSNumber numberWithBool:sameSiteInfo.isTopSite],
    };
#endif
    return policyProperties;
}
#endif

static RetainPtr<NSArray> cookiesForURLFromStorage(NSHTTPCookieStorage *storage, NSURL *url, NSURL *mainDocumentURL, const std::optional<SameSiteInfo>& sameSiteInfo, ThirdPartyCookieBlockingDecision thirdPartyCookieBlockingDecision, NSString *partition = nullptr)
{
    ASSERT(thirdPartyCookieBlockingDecision != ThirdPartyCookieBlockingDecision::All);

    // The _getCookiesForURL: method calls the completionHandler synchronously. We use std::optional<> to check this invariant and crash if it's not met.
    std::optional<RetainPtr<NSArray>> cookiesPtr;
    auto completionHandler = [&cookiesPtr] (NSArray *cookies) {
        cookiesPtr = retainPtr(cookies);
    };
#if defined(WEBKIT_IOS6)
    // This CFNetwork has neither SameSite nor partitioned cookies, and the
    // private accessor that carries those concepts does not exist on it. The
    // public API is the same query without them, and this OS enforces
    // third-party policy through the storage's accept policy instead.
    UNUSED_PARAM(mainDocumentURL);
    UNUSED_PARAM(sameSiteInfo);
    UNUSED_PARAM(partition);
    completionHandler([storage cookiesForURL:url]);
#else
    [storage _getCookiesForURL:url mainDocumentURL:mainDocumentURL partition:partition policyProperties:sameSiteInfo ? policyProperties(sameSiteInfo.value(), url, partition, thirdPartyCookieBlockingDecision).get() : nullptr completionHandler:completionHandler];
#endif
    RELEASE_ASSERT(!!cookiesPtr);

#if defined(WEBKIT_IOS6)
    // There are no storage partitions on this CFNetwork, so -_storagePartition
    // does not exist on NSHTTPCookie and there is nothing to filter by: every
    // cookie in this jar is unpartitioned and `partition` is always nil here,
    // because ENABLE(OPT_IN_PARTITIONED_COOKIES) is off on this port
    // (HAVE(ALLOW_ONLY_PARTITIONED_COOKIES) wants iOS 26.2). Hand back what the
    // jar returned.
    return WTF::move(*cookiesPtr);
#else
    // _getCookiesForURL returns only unpartitioned cookies if partition is nil, and it returns both
    // unpartitioned cookies plus cookies in the specified partition if partition is not nil. Return the
    // array of cookies the partition was nil, or if we should return both partitioned and unpartitioned
    // cookies
    if (!partition || thirdPartyCookieBlockingDecision == ThirdPartyCookieBlockingDecision::None)
        return WTF::move(*cookiesPtr);

    // Filter all cookies that aren't in the specified partition.
    RetainPtr<NSMutableArray<NSHTTPCookie *>> partitionedCookies = adoptNS([[NSMutableArray alloc] initWithCapacity:[cookiesPtr->get() count]]);
    for (NSHTTPCookie *nsCookie in cookiesPtr->get()) {
        if (![nsCookie._storagePartition isEqualToString:partition])
            continue;
        [partitionedCookies.get() addObject:nsCookie];
    }
    return WTF::move(partitionedCookies);
#endif
}

void NetworkStorageSession::setHTTPCookiesForURL(CFHTTPCookieStorageRef cookieStorage, NSArray *cookies, NSURL *url, NSURL *mainDocumentURL, NSString *partition, const SameSiteInfo& sameSiteInfo, ThirdPartyCookieBlockingDecision thirdPartyCookieBlockingDecision) const
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies) || m_isInMemoryCookieStore);

#if defined(WEBKIT_IOS6)
    // As above: no SameSite, no partitions, and no -_initWithCFHTTPCookieStorage:
    // either, so a per-session jar cannot be made. This CFNetwork has exactly one
    // cookie storage and the public setter writes to it. Without this, every
    // write raised, WebKit swallowed the exception, and document.cookie silently
    // did nothing - which is most of what makes a wrapped site fail to stay
    // logged in.
    UNUSED_PARAM(cookieStorage);
    UNUSED_PARAM(partition);
    UNUSED_PARAM(sameSiteInfo);
    UNUSED_PARAM(thirdPartyCookieBlockingDecision);
    [[NSHTTPCookieStorage sharedHTTPCookieStorage] setCookies:cookies forURL:url mainDocumentURL:mainDocumentURL];
#else
    if (!cookieStorage) {
        [[NSHTTPCookieStorage sharedHTTPCookieStorage] _setCookies:cookies forURL:url mainDocumentURL:mainDocumentURL policyProperties:policyProperties(sameSiteInfo, url, partition, thirdPartyCookieBlockingDecision).get()];
        return;
    }

    // FIXME: Stop creating a new NSHTTPCookieStorage object each time we want to query the cookie jar.
    // NetworkStorageSession could instead keep a NSHTTPCookieStorage object for us.
    RetainPtr<NSHTTPCookieStorage> nsCookieStorage = wrapCookieStorage(cookieStorage);
    [nsCookieStorage _setCookies:cookies forURL:url mainDocumentURL:mainDocumentURL policyProperties:policyProperties(sameSiteInfo, url, partition, thirdPartyCookieBlockingDecision).get()];
#endif
}

RetainPtr<NSArray> NetworkStorageSession::httpCookiesForURL(CFHTTPCookieStorageRef cookieStorage, NSURL *firstParty, const std::optional<SameSiteInfo>& sameSiteInfo, NSURL *url, ThirdPartyCookieBlockingDecision thirdPartyCookieBlockingDecision) const
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies) || m_isInMemoryCookieStore);
    if (!cookieStorage) {
        RELEASE_ASSERT(!m_isInMemoryCookieStore);
        cookieStorage = _CFHTTPCookieStorageGetDefault(kCFAllocatorDefault);
    }

    // FIXME: Stop creating a new NSHTTPCookieStorage object each time we want to query the cookie jar.
    // NetworkStorageSession could instead keep a NSHTTPCookieStorage object for us.
    RetainPtr<NSHTTPCookieStorage> nsCookieStorage = wrapCookieStorage(cookieStorage);
#if ENABLE(OPT_IN_PARTITIONED_COOKIES) && defined(CFN_COOKIE_ACCEPTS_POLICY_PARTITION) && CFN_COOKIE_ACCEPTS_POLICY_PARTITION
    RetainPtr partitionKey = isOptInCookiePartitioningEnabled() ? cookiePartitionIdentifier(firstParty).createNSString() : nil;
#else
    RetainPtr<NSString> partitionKey;
#endif
    return cookiesForURLFromStorage(nsCookieStorage.get(), url, firstParty, sameSiteInfo, thirdPartyCookieBlockingDecision, partitionKey.get());
}

RetainPtr<NSHTTPCookie> NetworkStorageSession::capExpiryOfPersistentCookie(NSHTTPCookie *cookie, Seconds cap)
{
    if ([cookie isSessionOnly])
        return cookie;

    if (!cookie.expiresDate || cookie.expiresDate.timeIntervalSinceNow > cap.seconds()) {
        auto properties = adoptNS([[cookie properties] mutableCopy]);
        auto date = adoptNS([[NSDate alloc] initWithTimeIntervalSinceNow:cap.seconds()]);
        [properties setObject:date.get() forKey:NSHTTPCookieExpires];
        return adoptNS([[NSHTTPCookie alloc] initWithProperties:properties.get()]);
    }
    return cookie;
}

#if ENABLE(OPT_IN_PARTITIONED_COOKIES) && defined(CFN_COOKIE_ACCEPTS_POLICY_PARTITION) && CFN_COOKIE_ACCEPTS_POLICY_PARTITION
NSHTTPCookie *NetworkStorageSession::setCookiePartition(NSHTTPCookie *cookie, NSString* partitionKey)
{
    if (!cookie)
        return cookie;

    if (!partitionKey)
        return cookie;

    if (cookie._storagePartition) {
        ASSERT(cookie._storagePartition == partitionKey);
        return cookie;
    }

    auto properties = adoptNS([[cookie properties] mutableCopy]);
    [properties setObject:partitionKey forKey:@"StoragePartition"];
    return [NSHTTPCookie cookieWithProperties:properties.get()];
}
#endif

RetainPtr<NSArray> NetworkStorageSession::cookiesForURL(const URL& firstParty, const SameSiteInfo& sameSiteInfo, const URL& url, std::optional<FrameIdentifier> frameID, std::optional<PageIdentifier> pageID, ApplyTrackingPrevention applyTrackingPrevention, ShouldRelaxThirdPartyCookieBlocking shouldRelaxThirdPartyCookieBlocking, IsKnownCrossSiteTracker isKnownCrossSiteTracker) const
{
    auto thirdPartyCookieBlockingDecision = thirdPartyCookieBlockingDecisionForRequest(firstParty, url, frameID, pageID, shouldRelaxThirdPartyCookieBlocking, isKnownCrossSiteTracker);
    if (applyTrackingPrevention == ApplyTrackingPrevention::Yes && thirdPartyCookieBlockingDecision == ThirdPartyCookieBlockingDecision::All)
        return nil;
    return httpCookiesForURL(cookieStorage().get(), firstParty.createNSURL().get(), sameSiteInfo, url.createNSURL().get(), thirdPartyCookieBlockingDecision);
}

std::pair<String, bool> NetworkStorageSession::cookiesForSession(const URL& firstParty, const SameSiteInfo& sameSiteInfo, const URL& url, std::optional<FrameIdentifier> frameID, std::optional<PageIdentifier> pageID, CookiesFor cookiesFor, IncludeSecureCookies includeSecureCookies, ApplyTrackingPrevention applyTrackingPrevention, ShouldRelaxThirdPartyCookieBlocking shouldRelaxThirdPartyCookieBlocking, IsKnownCrossSiteTracker isKnownCrossSiteTracker) const
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies) || m_isInMemoryCookieStore);

    BEGIN_BLOCK_OBJC_EXCEPTIONS

    auto cookies = cookiesForURL(firstParty, sameSiteInfo, url, frameID, pageID, applyTrackingPrevention, shouldRelaxThirdPartyCookieBlocking, isKnownCrossSiteTracker);
    if (![cookies count])
        return { String(), false }; // Return a null string; StringBuilder below would create an empty one.

    StringBuilder cookiesBuilder;
    bool didAccessSecureCookies = false;
    for (NSHTTPCookie *cookie in cookies.get()) {
        if (![[cookie name] length])
            continue;
        if (cookiesFor == CookiesFor::DOM && [cookie isHTTPOnly])
            continue;
        if ([cookie isSecure]) {
            didAccessSecureCookies = true;
            if (includeSecureCookies == IncludeSecureCookies::No)
                continue;
        }
        cookiesBuilder.append(cookiesBuilder.isEmpty() ? ""_s : "; "_s, [cookie name], '=', [cookie value]);
    }
    return { cookiesBuilder.toString(), didAccessSecureCookies };

    END_BLOCK_OBJC_EXCEPTIONS
    return { String(), false };
}

std::optional<Vector<Cookie>> NetworkStorageSession::cookiesForSessionAsVector(const URL& firstParty, const SameSiteInfo& sameSiteInfo, const URL& url, std::optional<FrameIdentifier> frameID, std::optional<PageIdentifier> pageID, CookiesFor cookiesFor, IncludeSecureCookies includeSecureCookies, ApplyTrackingPrevention applyTrackingPrevention, ShouldRelaxThirdPartyCookieBlocking shouldRelaxThirdPartyCookieBlocking, IsKnownCrossSiteTracker isKnownCrossSiteTracker, CookieStoreGetOptions&& options) const
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies) || m_isInMemoryCookieStore);

    BEGIN_BLOCK_OBJC_EXCEPTIONS

    auto cookies = cookiesForURL(firstParty, sameSiteInfo, url, frameID, pageID, applyTrackingPrevention, shouldRelaxThirdPartyCookieBlocking, isKnownCrossSiteTracker);
    if (![cookies count])
        return Vector<Cookie> { };

    Vector<Cookie> cookiesVector;
    RetainPtr name = options.name.createNSString();
    for (NSHTTPCookie *cookie in cookies.get()) {
        if (![[cookie name] length])
            continue;
        if (cookiesFor == CookiesFor::DOM && [cookie isHTTPOnly])
            continue;
        if ([cookie isSecure] && includeSecureCookies == IncludeSecureCookies::No)
            continue;
        if (!options.name.isNull() && ![[cookie name] isEqualToString:name.get()])
            continue;

        cookiesVector.append(Cookie(cookie));
    }
    return cookiesVector;

    END_BLOCK_OBJC_EXCEPTIONS
    return std::nullopt;
}

std::pair<String, bool> NetworkStorageSession::cookiesForDOM(const URL& firstParty, const SameSiteInfo& sameSiteInfo, const URL& url, std::optional<FrameIdentifier> frameID, std::optional<PageIdentifier> pageID, IncludeSecureCookies includeSecureCookies, ApplyTrackingPrevention applyTrackingPrevention, ShouldRelaxThirdPartyCookieBlocking shouldRelaxThirdPartyCookieBlocking, IsKnownCrossSiteTracker isKnownCrossSiteTracker) const
{
    return cookiesForSession(firstParty, sameSiteInfo, url, frameID, pageID, CookiesFor::DOM, includeSecureCookies, applyTrackingPrevention, shouldRelaxThirdPartyCookieBlocking, isKnownCrossSiteTracker);
}

std::optional<Vector<Cookie>> NetworkStorageSession::cookiesForDOMAsVector(const URL& firstParty, const SameSiteInfo& sameSiteInfo, const URL& url, std::optional<FrameIdentifier> frameID, std::optional<PageIdentifier> pageID, IncludeSecureCookies includeSecureCookies, ApplyTrackingPrevention applyTrackingPrevention, ShouldRelaxThirdPartyCookieBlocking shouldRelaxThirdPartyCookieBlocking, IsKnownCrossSiteTracker isKnownCrossSiteTracker, CookieStoreGetOptions&& options) const
{
    return cookiesForSessionAsVector(firstParty, sameSiteInfo, url, frameID, pageID, CookiesFor::DOM, includeSecureCookies, applyTrackingPrevention, shouldRelaxThirdPartyCookieBlocking, isKnownCrossSiteTracker, WTF::move(options));
}

std::pair<String, bool> NetworkStorageSession::cookieRequestHeaderFieldValue(const URL& firstParty, const SameSiteInfo& sameSiteInfo, const URL& url, std::optional<FrameIdentifier> frameID, std::optional<PageIdentifier> pageID, IncludeSecureCookies includeSecureCookies, ApplyTrackingPrevention applyTrackingPrevention, ShouldRelaxThirdPartyCookieBlocking shouldRelaxThirdPartyCookieBlocking, IsKnownCrossSiteTracker isKnownCrossSiteTracker) const
{
    return cookiesForSession(firstParty, sameSiteInfo, url, frameID, pageID, CookiesFor::HTTP, includeSecureCookies, applyTrackingPrevention, shouldRelaxThirdPartyCookieBlocking, isKnownCrossSiteTracker);
}

std::pair<String, bool> NetworkStorageSession::cookieRequestHeaderFieldValue(const CookieRequestHeaderFieldProxy& headerFieldProxy) const
{
    return cookiesForSession(headerFieldProxy.firstParty, headerFieldProxy.sameSiteInfo, headerFieldProxy.url, headerFieldProxy.frameID, headerFieldProxy.pageID, CookiesFor::HTTP, headerFieldProxy.includeSecureCookies, ApplyTrackingPrevention::Yes, ShouldRelaxThirdPartyCookieBlocking::No, IsKnownCrossSiteTracker::No);
}

static RetainPtr<NSHTTPCookie> adjustScriptWrittenCookie(NSHTTPCookie *initialCookie, std::optional<Seconds> cappedLifetime)
{
    if (!initialCookie)
        return nil;

#if ENABLE(JS_COOKIE_CHECKING)
    RetainPtr mutableProperties = adoptNS([[initialCookie properties] mutableCopy]);
    [mutableProperties.get() setValue:@1 forKey:@"SetInJavaScript"];
    RetainPtr cookie = adoptNS([[NSHTTPCookie alloc] initWithProperties:mutableProperties.get()]);
#else
    RetainPtr cookie = initialCookie;
#endif

    // <rdar://problem/5632883> On 10.5, NSHTTPCookieStorage would store an empty cookie,
    // which would be sent as "Cookie: =". We have a workaround in setCookies() to prevent
    // that, but we also need to avoid sending cookies that were previously stored, and
    // there's no harm to doing this check because such a cookie is never valid.
    if (![[cookie name] length])
        return nil;

    if ([cookie isHTTPOnly])
        return nil;

    // Cap lifetime of persistent, client-side cookies.
    if (cappedLifetime)
        return NetworkStorageSession::capExpiryOfPersistentCookie(cookie.get(), *cappedLifetime);

    return cookie;
}

static RetainPtr<NSHTTPCookie> parseDOMCookie(String cookieString, NSURL* cookieURL, std::optional<Seconds> cappedLifetime, const String& partition)
{
    // <rdar://problem/5632883> On 10.5, NSHTTPCookieStorage would store an empty cookie,
    // which would be sent as "Cookie: =".
    if (cookieString.isEmpty())
        return nil;

    // <http://bugs.webkit.org/show_bug.cgi?id=6531>, <rdar://4409034>
    // cookiesWithResponseHeaderFields doesn't parse cookies without a value
    cookieString = cookieString.contains('=') ? cookieString : makeString(cookieString, '=');

#if defined(WEBKIT_IOS6)
    // +_cookieForSetCookieString:forURL:partition: does not exist on this
    // Foundation, and partitions do not exist on this CFNetwork at all. The
    // public parser takes the same string in the form it arrives in over the
    // wire, and the two lines above already put it in the shape it wants.
    // Without this, assigning to document.cookie raised, WebKit swallowed the
    // exception, and the assignment silently did nothing.
    UNUSED_PARAM(partition);
    NSArray<NSHTTPCookie *> *parsed = [NSHTTPCookie
        cookiesWithResponseHeaderFields:@{ @"Set-Cookie": cookieString.createNSString().get() }
                                 forURL:cookieURL];
    return adjustScriptWrittenCookie([parsed firstObject], cappedLifetime);
#else
    return adjustScriptWrittenCookie([NSHTTPCookie _cookieForSetCookieString:cookieString.createNSString().get() forURL:cookieURL partition:nsStringNilIfEmpty(partition).get()], cappedLifetime);
#endif
}

void NetworkStorageSession::setCookiesFromDOM(const URL& firstParty, const SameSiteInfo& sameSiteInfo, const URL& url, std::optional<FrameIdentifier> frameID, std::optional<PageIdentifier> pageID, ApplyTrackingPrevention applyTrackingPrevention, RequiresScriptTrackingPrivacy requiresScriptTrackingPrivacy, const String& cookieString, ShouldRelaxThirdPartyCookieBlocking shouldRelaxThirdPartyCookieBlocking, IsKnownCrossSiteTracker isKnownCrossSiteTracker) const
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies) || m_isInMemoryCookieStore);

    BEGIN_BLOCK_OBJC_EXCEPTIONS

    auto thirdPartyCookieBlockingDecision = thirdPartyCookieBlockingDecisionForRequest(firstParty, url, frameID, pageID, shouldRelaxThirdPartyCookieBlocking, isKnownCrossSiteTracker);
    if (applyTrackingPrevention == ApplyTrackingPrevention::Yes && shouldBlockCookies(thirdPartyCookieBlockingDecision))
        return;

    RetainPtr cookieURL = url.createNSURL();

    auto cookieCap = clientSideCookieCap(RegistrableDomain { firstParty }, requiresScriptTrackingPrivacy, pageID);

#if ENABLE(OPT_IN_PARTITIONED_COOKIES) && defined(CFN_COOKIE_ACCEPTS_POLICY_PARTITION) && CFN_COOKIE_ACCEPTS_POLICY_PARTITION
    String partitionKey = isOptInCookiePartitioningEnabled() ? cookiePartitionIdentifier(firstParty) : String { };
#else
    String partitionKey;
#endif

    RetainPtr cookie = parseDOMCookie(cookieString, cookieURL.get(), cookieCap, partitionKey);
    if (!cookie)
        return;

    setHTTPCookiesForURL(cookieStorage().get(), @[cookie.get()], cookieURL.get(), firstParty.createNSURL().get(), nsStringNilIfEmpty(partitionKey).get(), sameSiteInfo, thirdPartyCookieBlockingDecision);

    END_BLOCK_OBJC_EXCEPTIONS
}

bool NetworkStorageSession::setCookieFromDOM(const URL& firstParty, const SameSiteInfo& sameSiteInfo, const URL& url, std::optional<FrameIdentifier> frameID, std::optional<PageIdentifier> pageID, ApplyTrackingPrevention applyTrackingPrevention, RequiresScriptTrackingPrivacy requiresScriptTrackingPrivacy, const Cookie& cookie, ShouldRelaxThirdPartyCookieBlocking shouldRelaxThirdPartyCookieBlocking, IsKnownCrossSiteTracker isKnownCrossSiteTracker) const
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies) || m_isInMemoryCookieStore);

    BEGIN_BLOCK_OBJC_EXCEPTIONS

    auto thirdPartyCookieBlockingDecision = thirdPartyCookieBlockingDecisionForRequest(firstParty, url, frameID, pageID, shouldRelaxThirdPartyCookieBlocking, isKnownCrossSiteTracker);
    if (applyTrackingPrevention == ApplyTrackingPrevention::Yes && shouldBlockCookies(thirdPartyCookieBlockingDecision))
        return false;

    auto expiryCap = clientSideCookieCap(RegistrableDomain { firstParty }, requiresScriptTrackingPrivacy, pageID);
    RetainPtr nshttpCookie = adjustScriptWrittenCookie(cookie.createNSHTTPCookie().get(), expiryCap);
    if (!nshttpCookie)
        return false;

#if ENABLE(OPT_IN_PARTITIONED_COOKIES) && defined(CFN_COOKIE_ACCEPTS_POLICY_PARTITION) && CFN_COOKIE_ACCEPTS_POLICY_PARTITION
    RetainPtr partition = isOptInCookiePartitioningEnabled() ? nsStringNilIfEmpty(cookiePartitionIdentifier(firstParty)) : nil;
#else
    RetainPtr<NSString> partition;
#endif

    setHTTPCookiesForURL(cookieStorage().get(), @[ nshttpCookie.get() ], url.createNSURL().get(), firstParty.createNSURL().get(), partition.get(), sameSiteInfo, thirdPartyCookieBlockingDecision);
    return true;

    END_BLOCK_OBJC_EXCEPTIONS
    return false;
}

static NSHTTPCookieAcceptPolicy httpCookieAcceptPolicy(CFHTTPCookieStorageRef cookieStorage)
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies));

    if (!cookieStorage)
        return [[NSHTTPCookieStorage sharedHTTPCookieStorage] cookieAcceptPolicy];

    return static_cast<NSHTTPCookieAcceptPolicy>(CFHTTPCookieStorageGetCookieAcceptPolicy(cookieStorage));
}

HTTPCookieAcceptPolicy NetworkStorageSession::cookieAcceptPolicy() const
{
    BEGIN_BLOCK_OBJC_EXCEPTIONS
    auto policy = httpCookieAcceptPolicy(cookieStorage().get());
    return toHTTPCookieAcceptPolicy(policy);
    END_BLOCK_OBJC_EXCEPTIONS

    return HTTPCookieAcceptPolicy::Never;
}

bool NetworkStorageSession::getRawCookies(const URL& firstParty, const SameSiteInfo& sameSiteInfo, const URL& url, std::optional<FrameIdentifier> frameID, std::optional<PageIdentifier> pageID, ApplyTrackingPrevention applyTrackingPrevention, ShouldRelaxThirdPartyCookieBlocking shouldRelaxThirdPartyCookieBlocking, Vector<Cookie>& rawCookies) const
{
    BEGIN_BLOCK_OBJC_EXCEPTIONS

    RetainPtr<NSArray> cookies = cookiesForURL(firstParty, sameSiteInfo, url, frameID, pageID, applyTrackingPrevention, shouldRelaxThirdPartyCookieBlocking, IsKnownCrossSiteTracker::No);
    NSUInteger count = [cookies count];
    rawCookies = Vector<Cookie>(count, [cookies](size_t i) {
        return Cookie { checked_objc_cast<NSHTTPCookie>([cookies objectAtIndex:i]) };
    });

    END_BLOCK_OBJC_EXCEPTIONS
    return true;
}

void NetworkStorageSession::deleteCookie(const URL& firstParty, const URL& url, const String& cookieName, CompletionHandler<void()>&& completionHandler) const
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies));

    auto aggregator = CallbackAggregator::create(WTF::move(completionHandler));
    
    BEGIN_BLOCK_OBJC_EXCEPTIONS

    RetainPtr<CFHTTPCookieStorageRef> cookieStorage = this->cookieStorage();
    RetainPtr<NSArray> cookies = httpCookiesForURL(cookieStorage.get(), firstParty.createNSURL().get(), std::nullopt, url.createNSURL().get(), ThirdPartyCookieBlockingDecision::None);

    RetainPtr cookieNameString = cookieName.createNSString();

    NSUInteger count = [cookies count];
    for (NSUInteger i = 0; i < count; ++i) {
        RetainPtr<NSHTTPCookie> cookie = [cookies objectAtIndex:i];
        if ([[cookie name] isEqualToString:cookieNameString.get()])
            deleteHTTPCookie(cookieStorage.get(), cookie.get(), [aggregator] { });
    }

    END_BLOCK_OBJC_EXCEPTIONS
}

void NetworkStorageSession::getHostnamesWithCookies(HashSet<String>& hostnames)
{
    BEGIN_BLOCK_OBJC_EXCEPTIONS

    RetainPtr<NSArray> cookies = httpCookies(cookieStorage().get());
    
    for (NSHTTPCookie* cookie in cookies.get()) {
        if (RetainPtr<NSString> domain = [cookie domain])
            hostnames.add(domain.get());
        else
            ASSERT_NOT_REACHED();
    }
    
    END_BLOCK_OBJC_EXCEPTIONS
}

void NetworkStorageSession::deleteAllCookies(CompletionHandler<void()>&& completionHandler)
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies));

    auto work = [completionHandler = WTF::move(completionHandler), cookieStorage = RetainPtr { cookieStorage() }] () mutable {
        if (!cookieStorage) {
            RetainPtr cookieStorage = [NSHTTPCookieStorage sharedHTTPCookieStorage];
            for (NSHTTPCookie *cookie in [cookieStorage cookies])
                [cookieStorage deleteCookie:cookie];
        } else
            CFHTTPCookieStorageDeleteAllCookies(cookieStorage.get());
        ensureOnMainThread(WTF::move(completionHandler));
    };
    
    if (m_isInMemoryCookieStore)
        return work();
    dispatch_async(globalDispatchQueueSingleton(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), makeBlockPtr(WTF::move(work)).get());
}

void NetworkStorageSession::deleteCookiesMatching(NOESCAPE const Function<bool(NSHTTPCookie *)>& matches, CompletionHandler<void()>&& completionHandler)
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies) || m_isInMemoryCookieStore);

    BEGIN_BLOCK_OBJC_EXCEPTIONS

    RetainPtr<CFHTTPCookieStorageRef> cookieStorage = this->cookieStorage();
    auto nsCookieStorage = wrapCookieStorage(cookieStorage.get());
    auto aggregator = CallbackAggregator::create([completionHandler = WTF::move(completionHandler), nsCookieStorage = WTF::move(nsCookieStorage)] () mutable {
#if defined(WEBKIT_IOS6)
        // -_saveCookies: does not exist on this Foundation and has no public
        // equivalent, because on this release there is nothing for a client to
        // ask for: CFNetwork owns writing Cookies.binarycookies and does it
        // itself. (The jar on the device already survives process launches, so
        // the writing is happening.) There is no flush to request, but the
        // completion handler still has to run on the main thread exactly as
        // _saveCookies:'s block would, or every caller of this hangs.
        UNUSED_PARAM(nsCookieStorage);
        ensureOnMainThread(WTF::move(completionHandler));
#else
        [nsCookieStorage _saveCookies:makeBlockPtr([completionHandler = WTF::move(completionHandler)] () mutable {
            ensureOnMainThread(WTF::move(completionHandler));
        }).get()];
#endif
    });

    RetainPtr<NSArray> cookies = httpCookies(cookieStorage.get());
    if (!cookies)
        return;

    for (NSHTTPCookie *cookie in cookies.get()) {
        @autoreleasepool {
            if (matches(cookie))
                deleteHTTPCookie(cookieStorage.get(), cookie, [aggregator] { });
        }
    }

    END_BLOCK_OBJC_EXCEPTIONS
}

void NetworkStorageSession::deleteCookies(const ClientOrigin& origin, CompletionHandler<void()>&& completionHandler)
{
#if defined(WEBKIT_IOS6)
    // -_storagePartition does not exist on NSHTTPCookie here because this
    // CFNetwork has no storage partitions: every cookie in the jar is
    // unpartitioned, so there is no partition half of this test to run and no
    // partition to read off a cookie. The domain is the whole test.
    auto domain = origin.clientOrigin.host();

    deleteCookiesMatching([&domain](auto *cookie) {
        return domain == String(cookie.domain);
    }, WTF::move(completionHandler));
#else
    Vector<String> cachePartitions { cookiePartitionIdentifier(origin.topOrigin.toURL()) };
    if (origin.topOrigin == origin.clientOrigin)
        cachePartitions.append({ });
    auto domain = origin.clientOrigin.host();

    deleteCookiesMatching([&domain, &cachePartitions](auto *cookie) {
        bool partitionMatched = std::ranges::any_of(cachePartitions, [&cookie](auto& cachePartition) {
            return equalIgnoringNullity(cachePartition, String(cookie._storagePartition));
        });
        return partitionMatched && domain == String(cookie.domain);
    }, WTF::move(completionHandler));
#endif
}

void NetworkStorageSession::deleteCookiesForHostnames(std::span<const String> hostnames, IncludeHttpOnlyCookies includeHttpOnlyCookies, ScriptWrittenCookiesOnly scriptWrittenCookiesOnly, CompletionHandler<void()>&& completionHandler)
{
    HashSet<String> hostnamesSet;
    for (auto& hostname : hostnames)
        hostnamesSet.add(hostname);

    deleteCookiesMatching([&](NSHTTPCookie* cookie) {
        if (!cookie.domain || (includeHttpOnlyCookies == IncludeHttpOnlyCookies::No && cookie.isHTTPOnly))
            return false;
#if ENABLE(JS_COOKIE_CHECKING)
        bool setInJS = [retainPtr([cookie properties]) valueForKey:@"SetInJavaScript"];
        if (scriptWrittenCookiesOnly == ScriptWrittenCookiesOnly::Yes && !setInJS)
            return false;
#else
        UNUSED_PARAM(scriptWrittenCookiesOnly);
#endif
        return hostnamesSet.contains(String(cookie.domain));
    }, WTF::move(completionHandler));
}

void NetworkStorageSession::deleteAllCookiesModifiedSince(WallTime timePoint, CompletionHandler<void()>&& completionHandler)
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanAccessRawCookies));

    // FIXME: Do we still need this check? Probably not.
    if (![NSHTTPCookieStorage instancesRespondToSelector:@selector(removeCookiesSinceDate:)])
        return completionHandler();

    NSTimeInterval timeInterval = timePoint.secondsSinceEpoch().seconds();
    auto work = [completionHandler = WTF::move(completionHandler), storage = RetainPtr { nsCookieStorage() }, date = RetainPtr { [NSDate dateWithTimeIntervalSince1970:timeInterval] }] () mutable {
        [storage removeCookiesSinceDate:date.get()];
#if defined(WEBKIT_IOS6)
        // As in deleteCookiesMatching: no -_saveCookies: on this Foundation and
        // no public equivalent, because CFNetwork writes the jar itself on this
        // release. Nothing to flush; run the handler so the caller finishes.
        ensureOnMainThread(WTF::move(completionHandler));
#else
        [storage _saveCookies:makeBlockPtr([completionHandler = WTF::move(completionHandler)] () mutable {
            ensureOnMainThread(WTF::move(completionHandler));
        }).get()];
#endif
    };

    if (m_isInMemoryCookieStore)
        return work();
    dispatch_async(globalDispatchQueueSingleton(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), makeBlockPtr(WTF::move(work)).get());
}

Vector<Cookie> NetworkStorageSession::domCookiesForHost(const URL& firstParty)
{
#if defined(WEBKIT_IOS6)
    // -_getCookiesForDomain: does not exist on this Foundation. The public
    // per-URL query is the same lookup addressed by URL instead of by bare host,
    // and it is the one CFNetwork itself uses to decide what applies to a
    // document loaded from that URL, so it additionally honours the path and
    // Secure rules the URL carries. This function's callers want the cookies the
    // DOM at that URL may see, so that narrowing is the correct answer rather
    // than a loss. There is no partitioned half to add either: this CFNetwork has
    // no partitions, so the whole jar is unpartitioned.
    RetainPtr nsCookies = [nsCookieStorage() cookiesForURL:firstParty.createNSURL().get()];
#else
    RetainPtr host = firstParty.host().createNSString();

    // _getCookiesForDomain only returned unpartitioned (i.e., nil partition) cookies
    RetainPtr<NSArray> unpartitionedCookies = [nsCookieStorage() _getCookiesForDomain:host.get()];
    RetainPtr nsCookies = adoptNS([[NSMutableArray alloc] initWithArray:unpartitionedCookies.get()]);

#if ENABLE(OPT_IN_PARTITIONED_COOKIES) && defined(CFN_COOKIE_ACCEPTS_POLICY_PARTITION) && CFN_COOKIE_ACCEPTS_POLICY_PARTITION
    if (isOptInCookiePartitioningEnabled()) {
        // Next, get all cookies in the partition for this site. However, we
        // only want the cookies for this host, so we filter all cookies that
        // don't match.
        // The _getCookiesForPartition: method calls the
        // completionHandler synchronously. We crash if this invariant is not
        // met.
        bool wasCompletionHandlerCalled { false };
        RetainPtr partitionKey = cookiePartitionIdentifier(firstParty).createNSString();
        auto completionHandler = [&wasCompletionHandlerCalled, &nsCookies, &host, &partitionKey, &firstParty] (NSArray *cookies) {
            wasCompletionHandlerCalled = true;

            RetainPtr registrableDomain = RegistrableDomain { firstParty }.string().createNSString();
            for (NSHTTPCookie *nsCookie in cookies) {
                if (![nsCookie.domain hasSuffix:registrableDomain.get()])
                    continue;
                if (![host hasSuffix:nsCookie.domain])
                    continue;

                ASSERT([nsCookie._storagePartition isEqualToString:partitionKey.get()]);
                if (![nsCookie._storagePartition isEqualToString:partitionKey.get()])
                    continue;

                [nsCookies addObject:nsCookie];
            }
        };

        [nsCookieStorage() _getCookiesForPartition:partitionKey.get() completionHandler:completionHandler];
        RELEASE_ASSERT(wasCompletionHandlerCalled);
    }
#endif
#endif

    return nsCookiesToCookieVector(nsCookies.get(), [](NSHTTPCookie *cookie) { return !cookie.HTTPOnly; });
}

#if ENABLE(OPT_IN_PARTITIONED_COOKIES)
void NetworkStorageSession::setOptInCookiePartitioningEnabled(bool enabled)
{
#if defined(CFN_COOKIE_ACCEPTS_POLICY_PARTITION) && CFN_COOKIE_ACCEPTS_POLICY_PARTITION
    m_isOptInCookiePartitioningEnabled = enabled;
#else
    RELEASE_ASSERT(m_thirdPartyCookieBlockingMode != WebCore::ThirdPartyCookieBlockingMode::AllExceptPartitioned);
    UNUSED_PARAM(enabled);
#endif
}
#endif

#if HAVE(COOKIE_CHANGE_LISTENER_API)

void NetworkStorageSession::registerCookieChangeListenersIfNecessary()
{
    if (m_didRegisterCookieListeners)
        return;

    m_didRegisterCookieListeners = true;

    [nsCookieStorage() _setCookiesChangedHandler:makeBlockPtr([weakThis = WeakPtr { *this }](NSArray<NSHTTPCookie *> *addedCookies, NSString *domainForChangedCookie) {
        CheckedPtr checkedThis = weakThis.get();
        if (!checkedThis)
            return;
        String host = domainForChangedCookie;
        auto it = checkedThis->m_cookieChangeObservers.find(host);
        if (it == checkedThis->m_cookieChangeObservers.end())
            return;
        auto cookies = nsCookiesToCookieVector(addedCookies, [](NSHTTPCookie *cookie) { return !cookie.HTTPOnly; });
        if (cookies.isEmpty())
            return;
        for (Ref observer : it->value)
            observer->cookiesAdded(host, cookies);
    }).get() onQueue:mainDispatchQueueSingleton()];

    [nsCookieStorage() _setCookiesRemovedHandler:makeBlockPtr([weakThis = WeakPtr { *this }](NSArray<NSHTTPCookie *> *removedCookies, NSString *domainForRemovedCookies, bool removeAllCookies) {
        CheckedPtr checkedThis = weakThis.get();
        if (!checkedThis)
            return;
        if (removeAllCookies) {
            for (auto& observers : checkedThis->m_cookieChangeObservers.values()) {
                for (Ref observer : observers)
                    observer->allCookiesDeleted();
            }
            return;
        }

        String host = domainForRemovedCookies;
        auto it = checkedThis->m_cookieChangeObservers.find(host);
        if (it == checkedThis->m_cookieChangeObservers.end())
            return;

        auto cookies = nsCookiesToCookieVector(removedCookies, [](NSHTTPCookie *cookie) { return !cookie.HTTPOnly; });
        if (cookies.isEmpty())
            return;
        for (Ref observer : it->value)
            observer->cookiesDeleted(host, cookies);
    }).get() onQueue:mainDispatchQueueSingleton()];
}

void NetworkStorageSession::unregisterCookieChangeListenersIfNecessary()
{
    if (!m_didRegisterCookieListeners)
        return;

    [nsCookieStorage() _setCookiesChangedHandler:nil onQueue:nil];
    [nsCookieStorage() _setCookiesRemovedHandler:nil onQueue:nil];

    [nsCookieStorage() _setSubscribedDomainsForCookieChanges:nil];
    m_didRegisterCookieListeners = false;
}

bool NetworkStorageSession::startListeningForCookieChangeNotifications(CookieChangeObserver& observer, const URL& url, const URL& firstParty, FrameIdentifier frameID, PageIdentifier pageID, ShouldRelaxThirdPartyCookieBlocking shouldRelaxThirdPartyCookieBlocking, IsKnownCrossSiteTracker isKnownCrossSiteTracker)
{
    if (shouldBlockCookies(firstParty, url, frameID, pageID, shouldRelaxThirdPartyCookieBlocking, isKnownCrossSiteTracker))
        return false;

    registerCookieChangeListenersIfNecessary();

    auto host = url.host().toString();
    auto& observers = m_cookieChangeObservers.ensure(host, [] {
        return WeakHashSet<CookieChangeObserver> { };
    }).iterator->value;

    observers.add(observer);

    if (!m_subscribedDomainsForCookieChanges)
        m_subscribedDomainsForCookieChanges = adoptNS([[NSMutableSet alloc] init]);
    else if ([m_subscribedDomainsForCookieChanges containsObject:host.createNSString().get()])
        return true;

    [m_subscribedDomainsForCookieChanges addObject:host.createNSString().get()];
    [nsCookieStorage() _setSubscribedDomainsForCookieChanges:m_subscribedDomainsForCookieChanges.get()];
    return true;
}

void NetworkStorageSession::stopListeningForCookieChangeNotifications(CookieChangeObserver& observer, const HashSet<String>& hosts)
{
    bool subscribedURLsChanged = false;
    for (auto& host : hosts) {
        auto it = m_cookieChangeObservers.find(host);
        ASSERT(it != m_cookieChangeObservers.end());
        if (it == m_cookieChangeObservers.end())
            continue;

        auto& observers = it->value;
        ASSERT(observers.contains(observer));
        observers.remove(observer);
        if (observers.isEmptyIgnoringNullReferences()) {
            m_cookieChangeObservers.remove(it);
            ASSERT([m_subscribedDomainsForCookieChanges containsObject:host.createNSString().get()]);
            [m_subscribedDomainsForCookieChanges removeObject:host.createNSString().get()];
            subscribedURLsChanged = true;
        }
    }
    if (subscribedURLsChanged)
        [nsCookieStorage() _setSubscribedDomainsForCookieChanges:m_subscribedDomainsForCookieChanges.get()];
}

#endif // HAVE(COOKIE_CHANGE_LISTENER_API)

} // namespace WebCore
