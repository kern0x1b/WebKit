/*
 * Copyright (C) 2023 Igalia S.L. All rights reserved.
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
#include "ContentVisibilityDocumentState.h"

#include "ContainerNodeInlines.h"
#include "ContentVisibilityAutoStateChangeEvent.h"
#include "DocumentTimeline.h"
#include "ElementInlinesLight.h"
#include "EventNames.h"
#include "FrameDestructionObserverInlines.h"
#include "FrameSelection.h"
#include "IntersectionObserverCallback.h"
#include "IntersectionObserverEntry.h"
#include "Logging.h"
#include "NodeDocument.h"
#include "NodeRenderStyle.h"
#include "RenderElement.h"
#include "Settings.h"
#include "SimpleRange.h"
#include "StyleComputedStyle+GettersInlines.h"
#include "StyleOriginatedAnimation.h"
#include "VisibleSelection.h"
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/TextStream.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ContentVisibilityDocumentState);

class ContentVisibilityIntersectionObserverCallback final : public IntersectionObserverCallback {
public:
    static Ref<ContentVisibilityIntersectionObserverCallback> create(Document& document)
    {
        return adoptRef(*new ContentVisibilityIntersectionObserverCallback(document));
    }

private:
    ContentVisibilityIntersectionObserverCallback(Document& document)
        : IntersectionObserverCallback(&document)
    {
    }

    bool hasCallback() const final { return true; }

    CallbackResult<void> invoke(IntersectionObserver&, const Vector<Ref<IntersectionObserverEntry>>& entries, IntersectionObserver&) final
    {
        ASSERT(!entries.isEmpty());

        for (auto& entry : entries) {
            if (RefPtr element = entry->target())
                protect(element->document())->contentVisibilityDocumentState().updateViewportProximity(*element, entry->isIntersecting() ? ViewportProximity::Near : ViewportProximity::Far);
        }
        return { };
    }

    CallbackResult<void> invokeRethrowingException(IntersectionObserver& thisObserver, const Vector<Ref<IntersectionObserverEntry>>& entries, IntersectionObserver& observer) final
    {
        return invoke(thisObserver, entries, observer);
    }
};

void ContentVisibilityDocumentState::observe(Element& element)
{
    Ref document = element.document();
    auto& state = document->contentVisibilityDocumentState();
    if (RefPtr intersectionObserver = state.intersectionObserver(document)) {
        intersectionObserver->observe(element);
#if defined(WEBKIT_IOS6)
        state.m_mayHaveTargetsWithoutViewportProximity = true;
#endif
    }
}

void ContentVisibilityDocumentState::unobserve(Element& element)
{
    Ref document = element.document();
    auto& state = document->contentVisibilityDocumentState();
    if (RefPtr intersectionObserver = state.m_observer) {
        intersectionObserver->unobserve(element);
        state.removeViewportProximity(element);
    }
    element.setContentRelevancy({ });
}

IntersectionObserver* ContentVisibilityDocumentState::intersectionObserver(Document& document)
{
    if (!m_observer) {
        auto callback = ContentVisibilityIntersectionObserverCallback::create(document);
#if defined(WEBKIT_IOS6)
        // A margin, so that "near the viewport" means near, not touching.
        //
        // Upstream creates this observer with no root margin at all, so anything
        // not literally intersecting the screen counts as far away and is
        // skipped. That is fine when content-visibility is something an author
        // asked for on a specific element; it is wrong when the engine applies it
        // by itself, because an infinite feed keeps its "load more" sentinel and
        // its prefetch observers in the content just below the fold - and a
        // skipped subtree is not observed. Measured on the device: with no margin
        // the document stopped growing at 1800-3000 px where it otherwise reaches
        // 4300-6500.
        //
        // The margin has to cover a flick, not a nudge. At two screens a fast
        // scroll outran it: sampled during a flick, one frame in twelve had
        // fourteen of the thirty boxes on screen still skipped, which is the white
        // space a reader sees. A flick moves about four hundred and thirty pixels
        // and several arrive before the observer has caught up, so the margin is
        // five screens - still far short of a feed that runs to ten thousand
        // pixels, which is where the layout saving comes from.
        IntersectionObserver::Init options { document, "500%"_s, { }, { } };
#else
        IntersectionObserver::Init options { document, { }, { }, { } };
#endif
        auto includeObscuredInsets = document.settings().contentInsetBackgroundFillEnabled() ? IncludeObscuredInsets::Yes : IncludeObscuredInsets::No;
        auto observer = IntersectionObserver::create(document, WTF::move(callback), WTF::move(options), includeObscuredInsets);
        if (observer.hasException())
            return nullptr;
        m_observer = observer.releaseReturnValue();
    }
    return m_observer.get();
}

bool ContentVisibilityDocumentState::checkRelevancyOfContentVisibilityElement(Element& target, OptionSet<ContentRelevancy> relevancyToCheck, const SimpleRange* selectionRange) const
{
    auto oldRelevancy = target.contentRelevancy();
    OptionSet<ContentRelevancy> newRelevancy;
    if (oldRelevancy)
        newRelevancy = *oldRelevancy;

    auto setRelevancyValue = [&](ContentRelevancy reason, bool value) {
        if (value)
            newRelevancy.add(reason);
        else
            newRelevancy.remove(reason);
    };

    if (relevancyToCheck.contains(ContentRelevancy::OnScreen)) {
        auto viewportProximityIterator = m_elementViewportProximities.find(target);
        auto viewportProximity = ViewportProximity::Far;
        if (viewportProximityIterator != m_elementViewportProximities.end())
            viewportProximity = viewportProximityIterator->value;
        setRelevancyValue(ContentRelevancy::OnScreen, viewportProximity == ViewportProximity::Near);
    }

    if (relevancyToCheck.contains(ContentRelevancy::Focused))
        setRelevancyValue(ContentRelevancy::Focused, target.hasFocusWithin());

    if (relevancyToCheck.contains(ContentRelevancy::Selected))
        setRelevancyValue(ContentRelevancy::Selected, selectionRange && intersects<ComposedTree>(*selectionRange, target));

    auto hasTopLayerinSubtree = [](const Element& target) {
        for (auto& element : target.document().topLayerElements()) {
            if (element->isDescendantOf(target))
                return true;
        }
        return false;
    };
    if (relevancyToCheck.contains(ContentRelevancy::IsInTopLayer))
        setRelevancyValue(ContentRelevancy::IsInTopLayer, hasTopLayerinSubtree(target));

    if (oldRelevancy && oldRelevancy == newRelevancy)
        return false;

    LOG_WITH_STREAM(ContentVisibility, stream << "ContentVisibilityDocumentState::checkRelevancyOfContentVisibilityElement - relevancy of " << target << " changed from " << oldRelevancy << " to " << newRelevancy);

    auto wasSkippedContent = target.isRelevantToUser() ? IsSkippedContent::No : IsSkippedContent::Yes;
    target.setContentRelevancy(newRelevancy);
    auto isSkippedContent = target.isRelevantToUser() ? IsSkippedContent::No : IsSkippedContent::Yes;
    target.invalidateStyle();
    updateAnimations(target, wasSkippedContent, isSkippedContent);
    Node::queueTaskKeepingNodeAlive(target, TaskSource::DOMManipulation, [isSkippedContent](auto& element) {
        if (!element.isConnected())
            return;

        ContentVisibilityAutoStateChangeEvent::Init init {
            { false, false, false },
            isSkippedContent == IsSkippedContent::Yes
        };
        element.dispatchEvent(ContentVisibilityAutoStateChangeEvent::create(eventNames().contentvisibilityautostatechangeEvent, WTF::move(init)));
    });
    return true;
}

DidUpdateAnyContentRelevancy ContentVisibilityDocumentState::updateRelevancyOfContentVisibilityElements(OptionSet<ContentRelevancy> relevancyToCheck) const
{
    auto didUpdateAnyContentRelevancy = DidUpdateAnyContentRelevancy::No;
    bool checksSelection = relevancyToCheck.contains(ContentRelevancy::Selected);
    bool didComputeSelectionRange = false;
    std::optional<SimpleRange> selectionRange;
    for (Ref target : m_observer->observationTargets()) {
        if (checksSelection && !didComputeSelectionRange) {
            didComputeSelectionRange = true;
            selectionRange = target->document().selection().selection().range();
        }
        if (checkRelevancyOfContentVisibilityElement(target, relevancyToCheck, selectionRange ? &*selectionRange : nullptr))
            didUpdateAnyContentRelevancy = DidUpdateAnyContentRelevancy::Yes;
    }
    return didUpdateAnyContentRelevancy;
}

HadInitialVisibleContentVisibilityDetermination ContentVisibilityDocumentState::determineInitialVisibleContentVisibility() const
{
    if (!m_observer)
        return HadInitialVisibleContentVisibilityDetermination::No;
#if defined(WEBKIT_IOS6)
    if (!m_mayHaveTargetsWithoutViewportProximity)
        return HadInitialVisibleContentVisibilityDetermination::No;
#endif
    Vector<Ref<Element>> elementsToCheck;
    for (Ref target : m_observer->observationTargets()) {
        bool checkForInitialDetermination = !m_elementViewportProximities.contains(target) && !target->isRelevantToUser();
        if (checkForInitialDetermination)
            elementsToCheck.append(target);
    }
    auto hadInitialVisibleContentVisibilityDetermination = HadInitialVisibleContentVisibilityDetermination::No;
#if defined(WEBKIT_IOS6)
    if (elementsToCheck.isEmpty())
        m_mayHaveTargetsWithoutViewportProximity = false;
#endif
    if (!elementsToCheck.isEmpty()) {
        Ref document = elementsToCheck.first()->document();
        if (protect(m_observer)->updateObservations(*protect(document->frame())) == IntersectionObserver::NeedNotify::Yes)
            protect(m_observer)->notify();

        for (auto& element : elementsToCheck) {
            checkRelevancyOfContentVisibilityElement(element, { ContentRelevancy::OnScreen }, nullptr);
            if (element->isRelevantToUser())
                hadInitialVisibleContentVisibilityDetermination = HadInitialVisibleContentVisibilityDetermination::Yes;
        }
    }
    return hadInitialVisibleContentVisibilityDetermination;
}

// Make sure any skipped content we want to scroll to is in the viewport, so it can be actually
// scrolled to (i.e. the skipped content early exit in LocalFrameView::scrollRectToVisible does
// not apply anymore).
void ContentVisibilityDocumentState::updateContentRelevancyForScrollIfNeeded(const Element& scrollAnchor)
{
    if (!m_observer)
        return;
    auto findSkippedContentRoot = [](const Element& element) -> RefPtr<const Element> {
        RefPtr<const Element> found;
        if (element.renderer() && element.renderer()->isSkippedContent()) {
            for (RefPtr candidate = element; candidate; candidate = candidate->parentElementInComposedTree()) {
                if (candidate->renderer() && candidate->renderStyle()->contentVisibility() == ContentVisibility::Auto)
                    found = candidate;
            }
        }
        return found;
    };

    if (RefPtr scrollAnchorRoot = findSkippedContentRoot(scrollAnchor)) {
        updateViewportProximity(*scrollAnchorRoot, ViewportProximity::Near);
        // Since we may not have determined initial visibility yet, force scheduling the content relevancy update.
        protect(scrollAnchorRoot->document())->scheduleContentRelevancyUpdate(ContentRelevancy::OnScreen);
        protect(scrollAnchorRoot->document())->updateRelevancyOfContentVisibilityElements();
    }
}

void ContentVisibilityDocumentState::updateViewportProximity(const Element& element, ViewportProximity viewportProximity)
{
    auto result = m_elementViewportProximities.ensure(element, [] {
        return ViewportProximity::Far;
    });
    // No need to schedule content relevancy update for first time call, since
    // that will be handled by determineInitialVisibleContentVisibility.
    if (!result.isNewEntry)
        protect(element.document())->scheduleContentRelevancyUpdate(ContentRelevancy::OnScreen);
    result.iterator->value = viewportProximity;
}

void ContentVisibilityDocumentState::removeViewportProximity(const Element& element)
{
    m_elementViewportProximities.remove(element);
}

void ContentVisibilityDocumentState::updateAnimations(const Element& element, IsSkippedContent wasSkipped, IsSkippedContent becomesSkipped)
{
    if (wasSkipped == IsSkippedContent::No || becomesSkipped == IsSkippedContent::Yes)
        return;
    for (auto& animation : WebAnimation::instances()) {
        RefPtr styleOriginatedAnimation = dynamicDowncast<StyleOriginatedAnimation>(animation.get());
        if (!styleOriginatedAnimation)
            continue;
        auto owningElement = styleOriginatedAnimation->owningElement();
        if (!owningElement || !owningElement->element.isShadowIncludingDescendantOf(&element))
            continue;

        if (RefPtr timeline = styleOriginatedAnimation->timeline())
            timeline->animationTimingDidChange(*styleOriginatedAnimation);
    }
}

}
