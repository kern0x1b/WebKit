/*
 * Copyright (C) 2014-2015 Apple Inc. All rights reserved.
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
#include "UserContentController.h"

#include "DOMWrapperWorld.h"
#include "UserScript.h"
#include "UserStyleSheet.h"
#include <JavaScriptCore/JSObjectInlines.h>

#if ENABLE(CONTENT_EXTENSIONS)
#include "CompiledContentExtension.h"
#include "ContentExtensionCompiler.h"
#include "ContentExtensionParser.h"
#include <wtf/URL.h>
#endif

namespace WebCore {

#if ENABLE(CONTENT_EXTENSIONS)

namespace {

class RuleListCompilationClient final : public ContentExtensions::ContentExtensionCompilationClient {
public:
    Vector<uint8_t> actions;
    Vector<uint8_t> urlFilters;
    Vector<uint8_t> topURLFilters;
    Vector<uint8_t> frameURLFilters;

private:
    void writeSource(String&&) final { }
    void writeActions(Vector<ContentExtensions::SerializedActionByte>&& bytes) final { actions.appendVector(bytes); }
    void writeURLFiltersBytecode(Vector<ContentExtensions::DFABytecode>&& bytecode) final { urlFilters.appendVector(bytecode); }
    void writeTopURLFiltersBytecode(Vector<ContentExtensions::DFABytecode>&& bytecode) final { topURLFilters.appendVector(bytecode); }
    void writeFrameURLFiltersBytecode(Vector<ContentExtensions::DFABytecode>&& bytecode) final { frameURLFilters.appendVector(bytecode); }
    void finalize() final { }
};

class InMemoryCompiledContentExtension final : public ContentExtensions::CompiledContentExtension {
public:
    static Ref<InMemoryCompiledContentExtension> create(RuleListCompilationClient&& client)
    {
        return adoptRef(*new InMemoryCompiledContentExtension(WTF::move(client)));
    }

private:
    explicit InMemoryCompiledContentExtension(RuleListCompilationClient&& client)
        : m_actions(WTF::move(client.actions))
        , m_urlFilters(WTF::move(client.urlFilters))
        , m_topURLFilters(WTF::move(client.topURLFilters))
        , m_frameURLFilters(WTF::move(client.frameURLFilters))
    {
    }

    std::span<const uint8_t> serializedActions() const final { return m_actions.span(); }
    std::span<const uint8_t> urlFiltersBytecode() const final { return m_urlFilters.span(); }
    std::span<const uint8_t> topURLFiltersBytecode() const final { return m_topURLFilters.span(); }
    std::span<const uint8_t> frameURLFiltersBytecode() const final { return m_frameURLFilters.span(); }

    Vector<uint8_t> m_actions;
    Vector<uint8_t> m_urlFilters;
    Vector<uint8_t> m_topURLFilters;
    Vector<uint8_t> m_frameURLFilters;
};

} // namespace

#endif // ENABLE(CONTENT_EXTENSIONS)

Ref<UserContentController> UserContentController::create()
{
    return adoptRef(*new UserContentController);
}

UserContentController::UserContentController() = default;

UserContentController::~UserContentController() = default;

void UserContentController::forEachUserScript(NOESCAPE const Function<void(DOMWrapperWorld&, const UserScript&)>& functor) const
{
    for (const auto& worldAndUserScriptVector : m_userScripts) {
        Ref world = worldAndUserScriptVector.key;
        for (const auto& userScript : *worldAndUserScriptVector.value)
            functor(world, *userScript);
    }
}

void UserContentController::forEachUserStyleSheet(NOESCAPE const Function<void(const UserStyleSheet&)>& functor) const
{
    for (auto& styleSheetVector : m_userStyleSheets.values()) {
        for (const auto& styleSheet : *styleSheetVector)
            functor(*styleSheet);
    }
}

#if ENABLE(USER_MESSAGE_HANDLERS)
void UserContentController::forEachUserMessageHandler(NOESCAPE const Function<void(const UserMessageHandlerDescriptor&)>&) const
{
}
#endif

void UserContentController::addUserScript(DOMWrapperWorld& world, std::unique_ptr<UserScript> userScript)
{
    auto& scriptsInWorld = m_userScripts.ensure(world, [&] { return makeUnique<UserScriptVector>(); }).iterator->value;
    scriptsInWorld->append(WTF::move(userScript));
}

void UserContentController::removeUserScript(DOMWrapperWorld& world, const URL& url)
{
    auto it = m_userScripts.find(world);
    if (it == m_userScripts.end())
        return;

    auto scripts = it->value.get();
    for (int i = scripts->size() - 1; i >= 0; --i) {
        if (scripts->at(i)->url() == url)
            scripts->removeAt(i);
    }

    if (scripts->isEmpty())
        m_userScripts.remove(it);
}

void UserContentController::removeUserScripts(DOMWrapperWorld& world)
{
    m_userScripts.remove(world);
}

void UserContentController::addUserStyleSheet(DOMWrapperWorld& world, std::unique_ptr<UserStyleSheet> userStyleSheet, UserStyleInjectionTime injectionTime)
{
    auto& styleSheetsInWorld = m_userStyleSheets.ensure(world, [&] { return makeUnique<UserStyleSheetVector>(); }).iterator->value;
    styleSheetsInWorld->append(WTF::move(userStyleSheet));

    if (injectionTime == InjectInExistingDocuments)
        invalidateInjectedStyleSheetCacheInAllFramesInAllPages();
}

void UserContentController::removeUserStyleSheet(DOMWrapperWorld& world, const URL& url)
{
    auto it = m_userStyleSheets.find(world);
    if (it == m_userStyleSheets.end())
        return;

    auto& stylesheets = *it->value;

    bool sheetsChanged = false;
    for (int i = stylesheets.size() - 1; i >= 0; --i) {
        if (stylesheets[i]->url() == url) {
            stylesheets.removeAt(i);
            sheetsChanged = true;
        }
    }

    if (!sheetsChanged)
        return;

    if (stylesheets.isEmpty())
        m_userStyleSheets.remove(it);

    invalidateInjectedStyleSheetCacheInAllFramesInAllPages();
}

void UserContentController::removeUserStyleSheets(DOMWrapperWorld& world)
{
    if (!m_userStyleSheets.remove(world))
        return;

    invalidateInjectedStyleSheetCacheInAllFramesInAllPages();
}

void UserContentController::removeAllUserContent()
{
    m_userScripts.clear();

    if (!m_userStyleSheets.isEmpty()) {
        m_userStyleSheets.clear();
        invalidateInjectedStyleSheetCacheInAllFramesInAllPages();
    }
}

#if ENABLE(CONTENT_EXTENSIONS)

bool UserContentController::addContentRuleList(const String& identifier, const String& ruleJSON)
{
    if (identifier.isEmpty())
        return false;

    auto parsedRules = ContentExtensions::parseRuleList(ruleJSON, ContentExtensions::CSSSelectorsAllowed::No);
    if (!parsedRules) {
        WTFLogAlways("CONTENTBLOCK parse failed: %s", parsedRules.error().message().c_str());
        return false;
    }

    RuleListCompilationClient client;
    if (auto error = ContentExtensions::compileRuleList(client, String { ruleJSON }, WTF::move(parsedRules.value()))) {
        WTFLogAlways("CONTENTBLOCK compile failed: %s", error.message().c_str());
        return false;
    }

    m_contentExtensionBackend.addContentExtension(identifier, InMemoryCompiledContentExtension::create(WTF::move(client)), URL { }, ContentExtensions::ContentExtension::ShouldCompileCSS::No);
    return true;
}

void UserContentController::removeContentRuleList(const String& identifier)
{
    m_contentExtensionBackend.removeContentExtension(identifier);
}

void UserContentController::removeAllContentRuleLists()
{
    m_contentExtensionBackend.removeAllContentExtensions();
}

#endif // ENABLE(CONTENT_EXTENSIONS)

} // namespace WebCore
