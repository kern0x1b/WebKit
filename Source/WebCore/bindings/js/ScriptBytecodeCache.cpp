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


#include "config.h"
#include "ScriptBytecodeCache.h"

#include <JavaScriptCore/BytecodeCacheError.h>
#include <JavaScriptCore/CachedBytecode.h>
#include <JavaScriptCore/CachedTypes.h>
#include <JavaScriptCore/JSCBytecodeCacheVersion.h>
#include <JavaScriptCore/JSCInlines.h>
#include <JavaScriptCore/UnlinkedFunctionExecutable.h>
#include <algorithm>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wtf/Assertions.h>
#include <wtf/FileHandle.h>
#include <wtf/FileSystem.h>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/SHA1.h>
#include <wtf/StdLibExtras.h>
#include <wtf/UUID.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

namespace WebCore {

static bool scriptBytecodeCacheChatterEnabled()
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = access("/tmp/native-bytecode-log", F_OK) == 0 ? 1 : 0;
    return enabled == 1;
}

static constexpr uint32_t bytecodeCacheMagic = 0x4a424332;
static constexpr uint32_t bytecodeCacheFormatVersion = 2;
static constexpr size_t bytecodeCacheMetaSize = 128;
static constexpr size_t bytecodeCacheBootUUIDSize = 40;
static constexpr size_t bytecodeCacheLastUsedOffset = 32;
static constexpr uint64_t bytecodeCacheMinimumBlobSize = 1024;
static size_t bytecodeCacheMinimumSourceLength()
{
    // Every cached script keeps its decoded form in memory, and on this device
    // that is the scarce thing: caching everything cost 31 MB of resident memory
    // for one and a half seconds of the launch. The site's weight is in a few
    // large bundles, so the floor is high by default and movable.
    static const size_t minimum = [] -> size_t {
        if (const char* override = getenv("WEBKIT_IOS6_BYTECODE_MINIMUM_BYTES")) {
            long value = atol(override);
            if (value > 0)
                return static_cast<size_t>(value);
        }
        return 512 * 1024;
    }();
    return minimum;
}

struct BytecodeCacheMeta {
    uint32_t magic { 0 };
    uint32_t formatVersion { 0 };
    uint32_t jscCacheVersion { 0 };
    uint32_t sourceHash { 0 };
    uint64_t sourceLength { 0 };
    uint64_t payloadSize { 0 };
    uint64_t lastUsed { 0 };
    uint64_t jscBuildIdentity { 0 };
    std::array<uint8_t, bytecodeCacheBootUUIDSize> bootUUID { };
    SHA1::Digest payloadDigest { };
};

static void putUint32(std::span<uint8_t> buffer, size_t offset, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i)
        buffer[offset + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
}

static void putUint64(std::span<uint8_t> buffer, size_t offset, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i)
        buffer[offset + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
}

static uint32_t takeUint32(std::span<const uint8_t> buffer, size_t offset)
{
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i)
        value |= static_cast<uint32_t>(buffer[offset + i]) << (8 * i);
    return value;
}

static uint64_t takeUint64(std::span<const uint8_t> buffer, size_t offset)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(buffer[offset + i]) << (8 * i);
    return value;
}

static uint64_t nowInSeconds()
{
    return static_cast<uint64_t>(time(nullptr));
}

static String appendPathComponent(const String& directory, const String& component)
{
    if (directory.isEmpty())
        return component;
    if (directory.endsWith('/'))
        return makeString(directory, component);
    return makeString(directory, '/', component);
}

static bool removePath(const String& path)
{
    auto utf8 = path.utf8();
    return !unlink(utf8.data());
}

static bool renamePath(const String& from, const String& to)
{
    auto fromUTF8 = from.utf8();
    auto toUTF8 = to.utf8();
    return !rename(fromUTF8.data(), toUTF8.data());
}

static std::optional<uint64_t> sizeOfPath(const String& path)
{
    auto utf8 = path.utf8();
    struct stat statBuffer { };
    if (stat(utf8.data(), &statBuffer))
        return std::nullopt;
    return static_cast<uint64_t>(statBuffer.st_size);
}

static bool createDirectories(const String& path)
{
    auto utf8 = path.utf8();
    struct stat statBuffer { };
    if (!stat(utf8.data(), &statBuffer))
        return S_ISDIR(statBuffer.st_mode);

    Vector<char> working(utf8.length() + 1);
    memcpySpan(working.mutableSpan().first(utf8.length()), byteCast<char>(utf8.span()));
    working[utf8.length()] = '\0';

    for (size_t i = 1; i < utf8.length(); ++i) {
        if (working[i] != '/')
            continue;
        working[i] = '\0';
        if (mkdir(working.span().data(), 0700) && errno != EEXIST)
            return false;
        working[i] = '/';
    }

    if (mkdir(working.span().data(), 0700) && errno != EEXIST)
        return false;

    return !stat(utf8.data(), &statBuffer) && S_ISDIR(statBuffer.st_mode);
}

static Vector<String> namesInDirectory(const String& path)
{
    Vector<String> names;
    auto utf8 = path.utf8();
    DIR* directory = opendir(utf8.data());
    if (!directory)
        return names;

    while (struct dirent* entry = readdir(directory)) {
        if (entry->d_name[0] == '.' && (!entry->d_name[1] || (entry->d_name[1] == '.' && !entry->d_name[2])))
            continue;
        names.append(String::fromUTF8(entry->d_name));
    }

    closedir(directory);
    return names;
}

static void touchLastUsed(const String& metaPath, uint64_t timestamp)
{
    auto utf8 = metaPath.utf8();
    int descriptor = open(utf8.data(), O_WRONLY | O_CLOEXEC);
    if (descriptor < 0)
        return;

    std::array<uint8_t, 8> encoded { };
    putUint64(std::span<uint8_t> { encoded }, 0, timestamp);
    ssize_t written = pwrite(descriptor, encoded.data(), encoded.size(), bytecodeCacheLastUsedOffset);
    UNUSED_VARIABLE(written);
    close(descriptor);
}

static std::array<uint8_t, bytecodeCacheBootUUIDSize> currentBootUUID()
{
    std::array<uint8_t, bytecodeCacheBootUUIDSize> result { };
    auto utf8 = bootSessionUUIDString().utf8();
    auto span = utf8.span();
    size_t length = std::min<size_t>(span.size(), bytecodeCacheBootUUIDSize);
    for (size_t i = 0; i < length; ++i)
        result[i] = static_cast<uint8_t>(span[i]);
    return result;
}

static Vector<uint8_t> encodeMeta(const BytecodeCacheMeta& meta)
{
    Vector<uint8_t> buffer(bytecodeCacheMetaSize);
    zeroSpan(buffer.mutableSpan());
    auto span = buffer.mutableSpan();
    putUint32(span, 0, meta.magic);
    putUint32(span, 4, meta.formatVersion);
    putUint32(span, 8, meta.jscCacheVersion);
    putUint32(span, 12, meta.sourceHash);
    putUint64(span, 16, meta.sourceLength);
    putUint64(span, 24, meta.payloadSize);
    putUint64(span, bytecodeCacheLastUsedOffset, meta.lastUsed);
    putUint64(span, 40, meta.jscBuildIdentity);
    for (size_t i = 0; i < bytecodeCacheBootUUIDSize; ++i)
        span[48 + i] = meta.bootUUID[i];
    for (size_t i = 0; i < SHA1::hashSize; ++i)
        span[88 + i] = meta.payloadDigest[i];
    return buffer;
}

static BytecodeCacheMeta decodeMeta(std::span<const uint8_t> buffer)
{
    BytecodeCacheMeta meta;
    meta.magic = takeUint32(buffer, 0);
    meta.formatVersion = takeUint32(buffer, 4);
    meta.jscCacheVersion = takeUint32(buffer, 8);
    meta.sourceHash = takeUint32(buffer, 12);
    meta.sourceLength = takeUint64(buffer, 16);
    meta.payloadSize = takeUint64(buffer, 24);
    meta.lastUsed = takeUint64(buffer, bytecodeCacheLastUsedOffset);
    meta.jscBuildIdentity = takeUint64(buffer, 40);
    for (size_t i = 0; i < bytecodeCacheBootUUIDSize; ++i)
        meta.bootUUID[i] = buffer[48 + i];
    for (size_t i = 0; i < SHA1::hashSize; ++i)
        meta.payloadDigest[i] = buffer[88 + i];
    return meta;
}

static std::optional<BytecodeCacheMeta> readMeta(const String& path)
{
    auto utf8 = path.utf8();
    int descriptor = open(utf8.data(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return std::nullopt;

    Vector<uint8_t> buffer(bytecodeCacheMetaSize);
    zeroSpan(buffer.mutableSpan());
    ssize_t bytesRead = read(descriptor, buffer.mutableSpan().data(), bytecodeCacheMetaSize);
    close(descriptor);

    if (bytesRead != static_cast<ssize_t>(bytecodeCacheMetaSize))
        return std::nullopt;

    return decodeMeta(buffer.span());
}

static SHA1::Digest digestOf(std::span<const uint8_t> bytes)
{
    SHA1 sha1;
    sha1.addBytes(bytes);
    SHA1::Digest digest;
    sha1.computeHash(digest);
    return digest;
}

static bool writeFileAtomically(const String& path, std::span<const uint8_t> bytes)
{
    String temporaryPath = makeString(path, ".tmp"_s);
    auto temporaryUTF8 = temporaryPath.utf8();
    removePath(temporaryPath);

    int descriptor = open(temporaryUTF8.data(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (descriptor < 0)
        return false;

    size_t written = 0;
    while (written < bytes.size()) {
        ssize_t result = write(descriptor, bytes.subspan(written).data(), bytes.size() - written);
        if (result <= 0) {
            close(descriptor);
            removePath(temporaryPath);
            return false;
        }
        written += static_cast<size_t>(result);
    }

    if (fsync(descriptor)) {
        close(descriptor);
        removePath(temporaryPath);
        return false;
    }
    close(descriptor);

    if (!renamePath(temporaryPath, path)) {
        removePath(temporaryPath);
        return false;
    }
    return true;
}

static String shortenedURL(const String& url)
{
    if (url.length() <= 120)
        return url;
    return makeString(url.left(60), "..."_s, url.right(57));
}

ScriptBytecodeCache& ScriptBytecodeCache::singleton()
{
    static NeverDestroyed<ScriptBytecodeCache> cache;
    return cache.get();
}

void ScriptBytecodeCache::setDirectory(const String& directory, uint64_t maximumSize)
{
    flush();

    if (directory.isEmpty()) {
        m_directory = String();
        m_maximumSize = 0;
        m_scanned = false;
        WTFLogAlways("BYTECODE disabled");
        return;
    }

    if (!createDirectories(directory)) {
        m_directory = String();
        WTFLogAlways("BYTECODE unusable directory %s", directory.utf8().data());
        return;
    }

    m_directory = directory;
    m_maximumSize = maximumSize;
    m_scanned = false;
    m_currentSize = 0;
    scanDirectory();

    WTFLogAlways("BYTECODE dir %s cap %llu MB used %llu KB purged %u jscVersion %u jscBuild %016llx boot '%s'",
        directory.utf8().data(),
        static_cast<unsigned long long>(maximumSize / (1024 * 1024)),
        static_cast<unsigned long long>(m_currentSize / 1024),
        m_purgedOnScan,
        JSC::computeJSCBytecodeCacheVersion(),
        static_cast<unsigned long long>(JSC::computeJSCBinaryIdentity()),
        bootSessionUUIDString().utf8().data());
}

void ScriptBytecodeCache::scanDirectory()
{
    if (m_scanned || m_directory.isEmpty())
        return;
    m_scanned = true;
    m_currentSize = 0;
    m_purgedOnScan = 0;

    auto names = namesInDirectory(m_directory);
    HashSet<String> stale;

    for (auto& name : names) {
        if (!name.endsWith(".meta"_s))
            continue;
        String key = name.left(name.length() - 5);
        auto meta = readMeta(appendPathComponent(m_directory, name));
        if (meta
            && meta->magic == bytecodeCacheMagic
            && meta->formatVersion == bytecodeCacheFormatVersion
            && meta->jscBuildIdentity == JSC::computeJSCBinaryIdentity()
            && meta->jscCacheVersion == JSC::computeJSCBytecodeCacheVersion()
            && meta->bootUUID == currentBootUUID())
            continue;
        stale.add(key);
    }

    for (auto& name : names) {
        String key;
        if (name.endsWith(".bc"_s))
            key = name.left(name.length() - 3);
        else if (name.endsWith(".meta"_s))
            key = name.left(name.length() - 5);

        String path = appendPathComponent(m_directory, name);
        if (key.isEmpty() || stale.contains(key)) {
            if (removePath(path) && name.endsWith(".meta"_s))
                ++m_purgedOnScan;
            continue;
        }

        auto size = sizeOfPath(path);
        if (size)
            m_currentSize += *size;
    }
}

void ScriptBytecodeCache::clear()
{
    for (auto* entry : copyToVector(m_entries))
        entry->discard();

    if (m_directory.isEmpty())
        return;

    unsigned removed = 0;
    for (auto& name : namesInDirectory(m_directory)) {
        if (removePath(appendPathComponent(m_directory, name)))
            ++removed;
    }
    m_currentSize = 0;
    WTFLogAlways("BYTECODE cleared %u files", removed);
}

void ScriptBytecodeCache::flush()
{
    if (m_entries.isEmpty())
        return;
    for (auto* entry : copyToVector(m_entries))
        entry->commit();
    reportTotals();
}

void ScriptBytecodeCache::reportTotals()
{
    WTFLogAlways("BYTECODE totals hits %u (%llu KB blob, %.0f ms load) misses %u (%llu KB source, %.0f ms compile) encoded %llu KB in %.0f ms committed %llu KB in %.0f ms disk %llu KB",
        m_hitCount, static_cast<unsigned long long>(m_hitBytes / 1024), m_loadTime.milliseconds(),
        m_missCount, static_cast<unsigned long long>(m_missSourceBytes / 1024), m_compileTime.milliseconds(),
        static_cast<unsigned long long>(m_encodedBytes / 1024), m_encodeTime.milliseconds(),
        static_cast<unsigned long long>(m_committedBytes / 1024), m_commitTime.milliseconds(),
        static_cast<unsigned long long>(m_currentSize / 1024));
}

void ScriptBytecodeCache::registerEntry(ScriptBytecodeCacheEntry& entry)
{
    m_entries.add(&entry);
}

void ScriptBytecodeCache::unregisterEntry(ScriptBytecodeCacheEntry& entry)
{
    m_entries.remove(&entry);
}

void ScriptBytecodeCache::didHit(uint64_t blobBytes, Seconds elapsed)
{
    ++m_hitCount;
    m_hitBytes += blobBytes;
    m_loadTime += elapsed;
}

void ScriptBytecodeCache::didMiss(uint64_t sourceBytes, Seconds compileTime)
{
    ++m_missCount;
    m_missSourceBytes += sourceBytes;
    m_compileTime += compileTime;
}

void ScriptBytecodeCache::didEncode(uint64_t blobBytes, Seconds elapsed)
{
    m_encodedBytes += blobBytes;
    m_encodeTime += elapsed;
}

void ScriptBytecodeCache::didCommit(uint64_t blobBytes, Seconds elapsed)
{
    m_committedBytes += blobBytes;
    m_commitTime += elapsed;
}

void ScriptBytecodeCache::noteWritten(uint64_t bytes)
{
    m_currentSize += bytes;
}

void ScriptBytecodeCache::evictIfNeeded()
{
    if (m_directory.isEmpty() || !m_maximumSize)
        return;

    scanDirectory();
    if (m_currentSize <= m_maximumSize)
        return;

    struct Candidate {
        String key;
        uint64_t lastUsed { 0 };
        uint64_t size { 0 };
    };

    HashMap<String, Candidate> candidates;
    for (auto& name : namesInDirectory(m_directory)) {
        String key;
        if (name.endsWith(".bc"_s))
            key = name.left(name.length() - 3);
        else if (name.endsWith(".meta"_s))
            key = name.left(name.length() - 5);
        else {
            removePath(appendPathComponent(m_directory, name));
            continue;
        }

        String path = appendPathComponent(m_directory, name);
        auto& candidate = candidates.add(key, Candidate { key, 0, 0 }).iterator->value;
        candidate.key = key;
        candidate.size += sizeOfPath(path).value_or(0);
        if (name.endsWith(".meta"_s)) {
            if (auto meta = readMeta(path))
                candidate.lastUsed = meta->lastUsed;
        }
    }

    auto ordered = copyToVector(candidates.values());
    std::sort(ordered.begin(), ordered.end(), [](const Candidate& a, const Candidate& b) {
        return a.lastUsed < b.lastUsed;
    });

    uint64_t target = m_maximumSize - m_maximumSize / 4;
    unsigned removed = 0;
    uint64_t removedBytes = 0;
    for (auto& candidate : ordered) {
        if (m_currentSize <= target)
            break;
        removePath(appendPathComponent(m_directory, makeString(candidate.key, ".bc"_s)));
        removePath(appendPathComponent(m_directory, makeString(candidate.key, ".meta"_s)));
        m_currentSize = candidate.size > m_currentSize ? 0 : m_currentSize - candidate.size;
        removedBytes += candidate.size;
        ++removed;
    }

    if (removed)
        WTFLogAlways("BYTECODE evict %u entries %llu KB, disk now %llu KB", removed, static_cast<unsigned long long>(removedBytes / 1024), static_cast<unsigned long long>(m_currentSize / 1024));
}

ScriptBytecodeCacheEntry::ScriptBytecodeCacheEntry(const JSC::SourceProvider& provider)
    : m_provider(provider)
{
}

ScriptBytecodeCacheEntry::~ScriptBytecodeCacheEntry()
{
    commit();
    if (m_registered)
        ScriptBytecodeCache::singleton().unregisterEntry(*this);
}

bool ScriptBytecodeCacheEntry::resolveKey()
{
    if (m_keyResolved)
        return !m_key.isEmpty();

    m_keyResolved = true;

    auto& cache = ScriptBytecodeCache::singleton();
    if (!cache.isEnabled())
        return false;

    const String& url = m_provider.sourceURL();
    if (url.isEmpty() || !url.startsWith("http"_s))
        return false;

    m_sourceHash = m_provider.hash();
    m_sourceLength = m_provider.source().length();
    if (m_sourceLength < bytecodeCacheMinimumSourceLength())
        return false;

    SHA1 sha1;
    sha1.addUTF8Bytes(url);
    sha1.addUTF8Bytes("\n"_s);
    sha1.addUTF8Bytes(String::number(m_sourceLength));
    sha1.addUTF8Bytes("\n"_s);
    sha1.addUTF8Bytes(String::number(m_sourceHash));
    SHA1::Digest digest;
    sha1.computeHash(digest);
    m_key = String::fromLatin1(SHA1::hexDigest(digest).data());

    if (!m_registered) {
        cache.registerEntry(*this);
        m_registered = true;
    }
    return !m_key.isEmpty();
}

RefPtr<JSC::CachedBytecode> ScriptBytecodeCacheEntry::load()
{
    if (m_cachedBytecode)
        return m_cachedBytecode;

    m_lookupStart = MonotonicTime::now();
    m_lookupStarted = true;

    auto& cache = ScriptBytecodeCache::singleton();
    if (!resolveKey())
        return nullptr;

    String base = appendPathComponent(cache.directory(), m_key);
    String metaPath = makeString(base, ".meta"_s);
    String payloadPath = makeString(base, ".bc"_s);

    auto reject = [&](const char* reason) -> RefPtr<JSC::CachedBytecode> {
        removePath(metaPath);
        removePath(payloadPath);
        if (scriptBytecodeCacheChatterEnabled())
            WTFLogAlways("BYTECODE miss %s reason %s", shortenedURL(m_provider.sourceURL()).utf8().data(), reason);
        return nullptr;
    };

    auto meta = readMeta(metaPath);
    if (!meta) {
        if (scriptBytecodeCacheChatterEnabled())
            WTFLogAlways("BYTECODE miss %s reason absent source %llu B", shortenedURL(m_provider.sourceURL()).utf8().data(), static_cast<unsigned long long>(m_sourceLength));
        return nullptr;
    }

    if (meta->magic != bytecodeCacheMagic)
        return reject("bad-magic");
    if (meta->formatVersion != bytecodeCacheFormatVersion)
        return reject("format-version");
    if (meta->jscBuildIdentity != JSC::computeJSCBinaryIdentity())
        return reject("jsc-build");
    if (meta->jscCacheVersion != JSC::computeJSCBytecodeCacheVersion())
        return reject("jsc-version");
    if (meta->bootUUID != currentBootUUID())
        return reject("boot-session");
    if (meta->sourceHash != m_sourceHash || meta->sourceLength != m_sourceLength)
        return reject("source-mismatch");
    if (!meta->payloadSize)
        return reject("empty-payload");

    auto payloadHandle = FileSystem::openFile(payloadPath, FileSystem::FileOpenMode::Read);
    if (!payloadHandle)
        return reject("absent-payload");

    auto payloadSize = payloadHandle.size();
    if (!payloadSize || *payloadSize != meta->payloadSize)
        return reject("payload-size");

    auto mapped = payloadHandle.map(FileSystem::MappedFileMode::Private);
    if (!mapped || mapped->size() != meta->payloadSize)
        return reject("map-failed");

    if (digestOf(mapped->span()) != meta->payloadDigest)
        return reject("digest-mismatch");

    m_cachedBytecode = JSC::CachedBytecode::create(WTF::move(*mapped));
    m_loadedFromDisk = true;

    Seconds elapsed = MonotonicTime::now() - m_lookupStart;
    cache.didHit(meta->payloadSize, elapsed);
    touchLastUsed(metaPath, nowInSeconds());
    if (scriptBytecodeCacheChatterEnabled()) {
        WTFLogAlways("BYTECODE hit %s blob %llu B source %llu B load %.1f ms",
            shortenedURL(m_provider.sourceURL()).utf8().data(),
            static_cast<unsigned long long>(meta->payloadSize),
            static_cast<unsigned long long>(m_sourceLength),
            elapsed.milliseconds());
    }
    return m_cachedBytecode;
}

void ScriptBytecodeCacheEntry::store(const JSC::BytecodeCacheGenerator& generator)
{
    auto& cache = ScriptBytecodeCache::singleton();
    if (!resolveKey())
        return;

    if (m_loadedFromDisk) {
        m_cachedBytecode = nullptr;
        m_loadedFromDisk = false;
        String rejectedBase = appendPathComponent(cache.directory(), m_key);
        removePath(makeString(rejectedBase, ".meta"_s));
        removePath(makeString(rejectedBase, ".bc"_s));
        if (scriptBytecodeCacheChatterEnabled())
            WTFLogAlways("BYTECODE reject %s: decode refused a loaded blob, regenerating", shortenedURL(m_provider.sourceURL()).utf8().data());
    }

    if (m_cachedBytecode && m_cachedBytecode->hasUpdates())
        return;

    Seconds compileTime = m_lookupStarted ? MonotonicTime::now() - m_lookupStart : 0_s;
    cache.didMiss(m_sourceLength, compileTime);

    MonotonicTime encodeStart = MonotonicTime::now();
    auto update = generator();
    if (!update || !update->size())
        return;

    uint64_t bytes = update->size();
    if (!m_cachedBytecode)
        m_cachedBytecode = JSC::CachedBytecode::create();
    m_cachedBytecode->addGlobalUpdate(update.releaseNonNull());

    Seconds encodeElapsed = MonotonicTime::now() - encodeStart;
    cache.didEncode(bytes, encodeElapsed);
    if (scriptBytecodeCacheChatterEnabled()) {
        WTFLogAlways("BYTECODE encode %s blob %llu B source %llu B compile %.1f ms encode %.1f ms",
            shortenedURL(m_provider.sourceURL()).utf8().data(),
            static_cast<unsigned long long>(bytes),
            static_cast<unsigned long long>(m_sourceLength),
            compileTime.milliseconds(), encodeElapsed.milliseconds());
    }
}

void ScriptBytecodeCacheEntry::discard()
{
    m_cachedBytecode = nullptr;
    m_loadedFromDisk = false;
}

void ScriptBytecodeCacheEntry::update(const JSC::UnlinkedFunctionExecutable* executable, JSC::CodeSpecializationKind kind, const JSC::UnlinkedFunctionCodeBlock* codeBlock)
{
    if (!m_cachedBytecode || !ScriptBytecodeCache::singleton().isEnabled())
        return;
    if (!m_cachedBytecode->leafExecutables().contains(executable))
        return;

    JSC::BytecodeCacheError error;
    RefPtr<JSC::CachedBytecode> functionBytecode = JSC::encodeFunctionCodeBlock(executable->vm(), codeBlock, error);
    if (!functionBytecode || error.isValid())
        return;
    m_cachedBytecode->addFunctionUpdate(executable, kind, functionBytecode.releaseNonNull());
}

void ScriptBytecodeCacheEntry::commit()
{
    if (!m_cachedBytecode)
        return;

    auto bytecode = WTF::move(m_cachedBytecode);
    m_cachedBytecode = nullptr;
    m_loadedFromDisk = false;

    auto& cache = ScriptBytecodeCache::singleton();
    if (!cache.isEnabled() || m_key.isEmpty())
        return;
    if (!bytecode->hasUpdates())
        return;

    size_t totalSize = bytecode->sizeForUpdate();
    if (totalSize < bytecodeCacheMinimumBlobSize)
        return;
    if (cache.maximumSize() && totalSize > cache.maximumSize() / 4)
        return;

    MonotonicTime start = MonotonicTime::now();

    Vector<uint8_t> buffer(totalSize);
    zeroSpan(buffer.mutableSpan());
    auto span = buffer.mutableSpan();
    auto base = bytecode->span();
    if (base.size() > totalSize)
        return;
    memcpySpan(span.first(base.size()), base);

    bool overflowed = false;
    bytecode->commitUpdates([&](off_t offset, std::span<const uint8_t> data) {
        if (offset < 0 || static_cast<uint64_t>(offset) + data.size() > totalSize) {
            overflowed = true;
            return;
        }
        memcpySpan(span.subspan(static_cast<size_t>(offset), data.size()), data);
    });

    if (overflowed) {
        WTFLogAlways("BYTECODE commit %s rejected: update out of range", shortenedURL(m_provider.sourceURL()).utf8().data());
        return;
    }

    String entryBase = appendPathComponent(cache.directory(), m_key);
    String payloadPath = makeString(entryBase, ".bc"_s);
    String metaPath = makeString(entryBase, ".meta"_s);

    BytecodeCacheMeta meta;
    meta.magic = bytecodeCacheMagic;
    meta.formatVersion = bytecodeCacheFormatVersion;
    meta.jscCacheVersion = JSC::computeJSCBytecodeCacheVersion();
    meta.jscBuildIdentity = JSC::computeJSCBinaryIdentity();
    meta.sourceHash = m_sourceHash;
    meta.sourceLength = m_sourceLength;
    meta.payloadSize = totalSize;
    meta.lastUsed = nowInSeconds();
    meta.bootUUID = currentBootUUID();
    meta.payloadDigest = digestOf(buffer.span());

    removePath(metaPath);
    if (!writeFileAtomically(payloadPath, buffer.span())) {
        WTFLogAlways("BYTECODE commit %s failed writing payload", shortenedURL(m_provider.sourceURL()).utf8().data());
        return;
    }
    if (!writeFileAtomically(metaPath, encodeMeta(meta).span())) {
        removePath(payloadPath);
        WTFLogAlways("BYTECODE commit %s failed writing meta", shortenedURL(m_provider.sourceURL()).utf8().data());
        return;
    }

    Seconds elapsed = MonotonicTime::now() - start;
    cache.noteWritten(totalSize + bytecodeCacheMetaSize);
    cache.didCommit(totalSize, elapsed);
    if (scriptBytecodeCacheChatterEnabled()) {
        WTFLogAlways("BYTECODE commit %s blob %llu B in %.1f ms",
            shortenedURL(m_provider.sourceURL()).utf8().data(),
            static_cast<unsigned long long>(totalSize), elapsed.milliseconds());
    }
    cache.evictIfNeeded();
}

} // namespace WebCore
