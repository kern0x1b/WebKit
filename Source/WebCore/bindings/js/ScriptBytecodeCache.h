/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
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

#include <JavaScriptCore/SourceProvider.h>
#include <wtf/HashSet.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/text/WTFString.h>

namespace JSC {
class CachedBytecode;
class SourceCode;
class UnlinkedFunctionCodeBlock;
class UnlinkedFunctionExecutable;
}

namespace WebCore {

class ScriptBytecodeCacheEntry {
    WTF_MAKE_NONCOPYABLE(ScriptBytecodeCacheEntry);
public:
    explicit ScriptBytecodeCacheEntry(const JSC::SourceProvider&);
    ~ScriptBytecodeCacheEntry();

    RefPtr<JSC::CachedBytecode> load();
    void store(const JSC::BytecodeCacheGenerator&);
    void update(const JSC::UnlinkedFunctionExecutable*, JSC::CodeSpecializationKind, const JSC::UnlinkedFunctionCodeBlock*);
    void commit();
    void discard();

private:
    bool resolveKey();

    const JSC::SourceProvider& m_provider;
    RefPtr<JSC::CachedBytecode> m_cachedBytecode;
    String m_key;
    unsigned m_sourceHash { 0 };
    uint64_t m_sourceLength { 0 };
    bool m_registered { false };
    bool m_keyResolved { false };
    bool m_loadedFromDisk { false };
    bool m_lookupStarted { false };
    MonotonicTime m_lookupStart;
};

class ScriptBytecodeCache {
public:
    WEBCORE_EXPORT static ScriptBytecodeCache& singleton();

    WEBCORE_EXPORT void setDirectory(const String&, uint64_t maximumSize);
    WEBCORE_EXPORT void clear();
    WEBCORE_EXPORT void flush();
    WEBCORE_EXPORT void reportTotals();

    bool isEnabled() const { return !m_directory.isEmpty(); }
    const String& directory() const { return m_directory; }
    uint64_t maximumSize() const { return m_maximumSize; }

    void registerEntry(ScriptBytecodeCacheEntry&);
    void unregisterEntry(ScriptBytecodeCacheEntry&);

    void didHit(uint64_t blobBytes, Seconds);
    void didMiss(uint64_t sourceBytes, Seconds compileTime);
    void didEncode(uint64_t blobBytes, Seconds);
    void didCommit(uint64_t blobBytes, Seconds);

    void noteWritten(uint64_t bytes);
    void evictIfNeeded();

private:
    friend class WTF::NeverDestroyed<ScriptBytecodeCache>;
    ScriptBytecodeCache() = default;

    void scanDirectory();

    String m_directory;
    uint64_t m_maximumSize { 0 };
    uint64_t m_currentSize { 0 };
    unsigned m_purgedOnScan { 0 };
    bool m_scanned { false };

    HashSet<ScriptBytecodeCacheEntry*> m_entries;

    unsigned m_hitCount { 0 };
    unsigned m_missCount { 0 };
    uint64_t m_hitBytes { 0 };
    uint64_t m_missSourceBytes { 0 };
    uint64_t m_encodedBytes { 0 };
    uint64_t m_committedBytes { 0 };
    Seconds m_loadTime;
    Seconds m_compileTime;
    Seconds m_encodeTime;
    Seconds m_commitTime;
};

} // namespace WebCore
